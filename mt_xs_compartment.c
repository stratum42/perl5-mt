/*    mt_xs_compartment.c
 *
 *    perl5-mt: XS compartments, the out-of-process tier of the XS
 *    dispatcher: the pool forked at the freeze, the shared rings, the
 *    service loop in the child and the compartment table in the host.
 *    The in-process tier's bookkeeping (the boot wrapper that stamps
 *    CVf_MT_THREADSAFE) and the MT::XS functions live here too.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_XS_COMPARTMENT_C
#include "perl.h"
#include "XSUB.h"
#include "mt_xs.h"
#include "mt_fiber.h"
#include "perliol.h"
#include "mt_cap.h"
#include "mt_fault.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static struct mt_xs_sym *S_sym_of(void *p);
static CV *S_mt_xs_install_remote_boot(pTHX_ const char *name, struct mt_xs_sym *rs);

/* ---- tier one: the boot wrapper ---------------------------------------- */

/* What the bootstrap CV that dl_install_xsub creates carries in its any_sv:
 * the real boot function and the module's package name. */
typedef struct mt_xs_boot {
    XSUBADDR_t real;
    char       pkg[1];           /* NUL terminated, allocated to fit */
} mt_xs_boot_t;

/*
=for apidoc_section $embedding

=for apidoc mt_xs_install_boot

Creates the C<Pkg::bootstrap> XSUB for a module C<DynaLoader> has just
loaded in-process (C<dl_install_xsub>).  The CV runs the real boot
function C<symref> through L</mt_xs_boot_wrapper>, which sets
C<PL_mt_boot_safe> around it when C<dl_find_symbol> found the module's
C<mt_threadsafe_boot_*> marker (C<PL_mt_boot_safe_sym>), so that every
XSUB the boot defines is stamped C<CVf_MT_THREADSAFE>, and records the
package in C<PL_mt_boot_pkg> while it runs.  Any other name is installed
as a plain XSUB.

=for apidoc mt_xs_boot_wrapper

The body of a bootstrap CV made by L</mt_xs_install_boot>.

=cut
*/

CV *
Perl_mt_xs_install_boot(pTHX_ const char *name, void *symref,
                        const char *filename)
{
    const STRLEN len = strlen(name);
    CV *cv;

    PERL_ARGS_ASSERT_MT_XS_INSTALL_BOOT;

    {
        mt_xs_sym_t * const rs = S_sym_of(symref);
        if (rs)
            return S_mt_xs_install_remote_boot(aTHX_ name, rs);
    }
    if (len <= 11 || !strEQ(name + len - 11, "::bootstrap")) {
        return newXS_flags(name, DPTR2FPTR(XSUBADDR_t, symref), filename,
                           NULL, XS_DYNAMIC_FILENAME);
    }
    else {
        const STRLEN pkglen = len - 11;
        SV * const box = newSV(sizeof(mt_xs_boot_t) + pkglen);
        mt_xs_boot_t * const b = (mt_xs_boot_t *)SvPVX(box);
        const bool safe = symref == PL_mt_boot_safe_sym && symref != NULL;

        SvPOK_only(box);
        SvCUR_set(box, sizeof(mt_xs_boot_t) + pkglen);
        b->real = DPTR2FPTR(XSUBADDR_t, symref);
        Copy(name, b->pkg, pkglen, char);
        b->pkg[pkglen] = '\0';

        cv = newXS_flags(name, Perl_mt_xs_boot_wrapper, filename, NULL,
                         XS_DYNAMIC_FILENAME);
        CvXSUBANY(cv).any_sv = box;
        CvREFCOUNTED_ANYSV_on(cv);
        if (safe)
            CvMT_THREADSAFE_on(cv);
        PL_mt_boot_safe_sym = NULL;
        return cv;
    }
}

void
Perl_mt_xs_boot_wrapper(pTHX_ CV *cv)
{
    const mt_xs_boot_t * const b =
        (const mt_xs_boot_t *)SvPVX(CvXSUBANY(cv).any_sv);
    const int saved_safe = PL_mt_boot_safe;
    const char * const saved_pkg = PL_mt_boot_pkg;

    PERL_ARGS_ASSERT_MT_XS_BOOT_WRAPPER;

    /* the savestack restores these if the boot dies; the explicit restore
     * below covers the normal return (the stack is handed through
     * untouched: the boot pops the mark itself) */
    SAVEINT(PL_mt_boot_safe);
    SAVEPPTR(PL_mt_boot_pkg);
    PL_mt_boot_safe = CvMT_THREADSAFE(cv) ? 1 : 0;
    PL_mt_boot_pkg = b->pkg;
    if (Perl_mt_xs_in_compartment() && PerlEnv_getenv("PERL_MT_XS_DEBUG"))
        PerlIO_printf(PerlIO_stderr(), "mt_xs[comp %u]: boot %s (%s)\n",
                      (unsigned)Perl_mt_xs_my_comp(), b->pkg, CvFILE(cv));
    b->real(aTHX_ cv);
    PL_mt_boot_safe = saved_safe;
    PL_mt_boot_pkg = saved_pkg;
}

/*
=for apidoc mt_xs_log_newxs

Called by C<newXS_len_flags> inside an XS compartment for every named XSUB
defined while a frame is served: the compartment reports them to the host,
which installs a remote stub for each.

=cut
*/

void
Perl_mt_xs_log_newxs(pTHX_ CV *cv)
{
    PERL_ARGS_ASSERT_MT_XS_LOG_NEWXS;
    if (PL_mt_xs_newxs_log)
        av_push(PL_mt_xs_newxs_log, SvREFCNT_inc_simple_NN(MUTABLE_SV(cv)));
}

/* ---- the pool ------------------------------------------------------------ */

/*
 * The pool is process-wide.  It is created by the main thread at the start
 * of perl_freeze_boot_image, before the image clone and before any worker
 * exists, so that every compartment is a fork of a single-threaded process
 * and never carries the image.  Workers only read S_pool after that.
 */

typedef struct mt_xs_loaded_mod {
    char    *module;
    uint32_t flags;
    void    *payload;
    size_t   payload_len;
    struct mt_xs_loaded_mod *next;
} mt_xs_loaded_mod_t;

typedef struct mt_xs_host_comp {
    perl_krt_comp_t     proc;
    uint32_t            gen;        /* mirrors the shared header's gen        */
    int                 dead;
    uint64_t            calls;
    uint32_t            restarts;
    uint64_t            last_restart_ns;
    uint64_t            failed_inflight;
    mt_xs_loaded_mod_t *loaded_mods;
    uint32_t            nloaded;
} mt_xs_host_comp_t;

typedef struct mt_xs_host_spare {
    perl_krt_comp_t proc;
    int             used;
    int             dead;
} mt_xs_host_spare_t;

#define MT_XS_MAX_SIBLINGS 16

typedef struct mt_xs_pool {
    int               started;
    int               child;         /* 1 + index of the compartment we are */
    pid_t             parent_pid;
    perl_krt_shm_t    shm;
    mt_xs_shm_hdr_t  *hdr;
    mt_xs_comp_hdr_t *comps;
    mt_xs_slot_hdr_t *slots;
    mt_xs_spare_hdr_t *spares;
    perl_krt_vectab_t *vec;          /* doorbells: see S_vec_comp/S_vec_slot */
    uint32_t          ncomp;
    uint32_t          nslots;        /* total slots including reload_slot    */
    uint32_t          worker_slots;  /* worker-allocatable slots             */
    uint32_t          reload_slot;   /* UINT32_MAX if no spares              */
    uint32_t          nspares;       /* PERL_MT_XS_SPARES                    */
    perl_krt_au32_t   stopping;
    size_t            arena_len;
    uint32_t          batch;         /* PERL_MT_XS_BATCH                    */
    uint32_t          inflight;      /* PERL_MT_XS_INFLIGHT                 */
    int               affinity;      /* PERL_MT_XS_AFFINITY=smt             */
    mt_xs_host_comp_t hc[MT_XS_MAX_COMPARTMENTS];
    mt_xs_host_spare_t hsp[MT_XS_MAX_SPARES];
    pthread_t         reaper[MT_XS_MAX_COMPARTMENTS];
    int               comp_cpu[MT_XS_MAX_COMPARTMENTS];   /* -1: not pinned  */
    int               nsib[MT_XS_MAX_COMPARTMENTS];
    int               sib[MT_XS_MAX_COMPARTMENTS][MT_XS_MAX_SIBLINGS];
    uint32_t          sib_next[MT_XS_MAX_COMPARTMENTS];  /* round robin    */
    uint32_t          slot_seq[MT_XS_MAX_SLOTS];   /* host: next call seq  */
    uint32_t          next_comp;                   /* host: round robin    */
    perl_mutex        mutex;
    perl_mutex        reload_mutex;
} mt_xs_pool_t;

static mt_xs_pool_t S_pool;

#define MT_XS_AT(off)  ((void *)((char *)S_pool.shm.base + (off)))
#define S_req_ring(i)   ((perl_krt_mpsc_t *)MT_XS_AT(S_pool.comps[i].req_ring_off))
#define S_reply_ring(s) ((perl_krt_spsc_t *)MT_XS_AT(S_pool.slots[s].reply_ring_off))
#define S_arena(s)      ((char *)MT_XS_AT(S_pool.slots[s].arena_off))

/* The doorbell vectors: compartment c rings vector c when a
 * request is pushed for it; slot s rings vector ncomp + s when a reply is
 * pushed for it; spare sp rings vector ncomp + nslots + sp on promotion. */
#define S_vec_comp(c)   ((uint32_t)(c))
#define S_vec_slot(s)   ((uint32_t)(S_pool.ncomp + (s)))
#define S_vec_spare(sp) ((uint32_t)(S_pool.ncomp + S_pool.nslots + (sp)))

/* The reaper: one thread per compartment, blocked in waitpid, so that a
 * death wakes every waiter bound to it at once rather than at the next
 * backstop, and promotes a pre-forked spare if available. */
static void S_mt_xs_wake_comp(uint32_t comp);
static void S_mt_xs_mark_dead_locked(uint32_t comp, int wait_status);
static int  S_mt_xs_promote_spare(uint32_t comp);

static void *
S_mt_xs_reaper(void *arg)
{
    const uint32_t comp = (uint32_t)(uintptr_t)arg;
    for (;;) {
        pid_t pid;
        int st = 0, rc;

        MUTEX_LOCK(&S_pool.mutex);
        if (atomic_load_explicit(&S_pool.stopping, memory_order_acquire)
            || S_pool.hc[comp].dead || S_pool.hc[comp].proc.pid <= 0) {
            MUTEX_UNLOCK(&S_pool.mutex);
            return NULL;
        }
        pid = S_pool.hc[comp].proc.pid;
        MUTEX_UNLOCK(&S_pool.mutex);

        do {
            rc = waitpid(pid, &st, 0);
        } while (rc < 0 && errno == EINTR);

        if (atomic_load_explicit(&S_pool.stopping, memory_order_acquire))
            return NULL;

        MUTEX_LOCK(&S_pool.mutex);
        if (S_pool.hc[comp].proc.pid == pid && !S_pool.hc[comp].dead)
            S_mt_xs_mark_dead_locked(comp, st);
        MUTEX_UNLOCK(&S_pool.mutex);

        if (S_mt_xs_promote_spare(comp))
            continue;
        return NULL;
    }
}

/* A short pause for the one case with no vector to wait on (a compartment
 * writing into a full reply ring) */
static void
S_mt_xs_pause(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50000;
    (void)nanosleep(&ts, NULL);
}

static size_t
S_round_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

static long
S_env_long(pTHX_ const char *name, long dflt)
{
    const char * const e = PerlEnv_getenv(name);
    return e && *e ? atol(e) : dflt;
}

static int S_mt_xs_comp_main(void *arg);
static int S_mt_xs_spare_main(void *arg);

/*
=for apidoc mt_xs_pool_start

Creates the compartment pool: the shared segment (frame rings and
per-worker arenas), C<PERL_MT_XS_COMPARTMENTS> forked children (two by
default; zero disables the out-of-process tier), and C<PERL_MT_XS_SPARES>
pre-forked spare compartments (one by default) for restart without forking
after worker threads exist.  Called once by C<perl_freeze_boot_image> in
the main thread before any worker exists; later calls do nothing.

=for apidoc mt_xs_pool_stop

Terminates and reaps the compartments and spares (at the main interpreter's
destruction).

=cut
*/

void
Perl_mt_xs_pool_start(pTHX)
{
    long ncomp, nslots, nspares, arena_mb;
    uint32_t total_slots;
    size_t off, page, total;
    uint32_t i;

    if (S_pool.started || S_pool.child)
        return;
    S_pool.started = 1;
    MUTEX_INIT(&S_pool.mutex);
    MUTEX_INIT(&S_pool.reload_mutex);
    atomic_init(&S_pool.stopping, 0u);
    ncomp = S_env_long(aTHX_ "PERL_MT_XS_COMPARTMENTS", MT_XS_DEFAULT_COMPARTMENTS);
    if (ncomp <= 0)
        return;
    if (ncomp > MT_XS_MAX_COMPARTMENTS)
        ncomp = MT_XS_MAX_COMPARTMENTS;
    nspares = S_env_long(aTHX_ "PERL_MT_XS_SPARES", MT_XS_DEFAULT_SPARES);
    if (nspares < 0) nspares = 0;
    if (nspares > MT_XS_MAX_SPARES) nspares = MT_XS_MAX_SPARES;
    nslots = S_env_long(aTHX_ "PERL_MT_XS_SLOTS", MT_XS_DEFAULT_SLOTS);
    if (nslots < 1) nslots = 1;
    if (nslots > MT_XS_MAX_SLOTS - (nspares > 0 ? 1 : 0))
        nslots = MT_XS_MAX_SLOTS - (nspares > 0 ? 1 : 0);
    total_slots = (uint32_t)nslots + (nspares > 0 ? 1u : 0u);
    arena_mb = S_env_long(aTHX_ "PERL_MT_XS_ARENA_MB", MT_XS_DEFAULT_ARENA_MB);
    if (arena_mb < 1) arena_mb = 1;
    S_pool.batch = (uint32_t)S_env_long(aTHX_ "PERL_MT_XS_BATCH", MT_XS_DEFAULT_BATCH);
    if (S_pool.batch < 1) S_pool.batch = 1;
    S_pool.inflight = (uint32_t)S_env_long(aTHX_ "PERL_MT_XS_INFLIGHT",
                                           MT_XS_DEFAULT_INFLIGHT);
    if (S_pool.inflight < 1) S_pool.inflight = 1;
    {
        const char * const aff = PerlEnv_getenv("PERL_MT_XS_AFFINITY");
        S_pool.affinity = aff && strEQ(aff, "smt");
    }

    page = perl_krt_page_size();
    S_pool.arena_len = (size_t)arena_mb << 20;

    /* the layout: header, compartment headers, slot headers, spare headers,
     * vector table, one request ring per compartment, then one reply ring
     * and one arena per slot */
    off = S_round_up(sizeof(mt_xs_shm_hdr_t), PERL_KRT_CACHE_LINE);
    {
        const size_t comp_hdr_off = off;
        const size_t slot_hdr_off = comp_hdr_off
                                  + (size_t)ncomp * sizeof(mt_xs_comp_hdr_t);
        const size_t spare_hdr_off = S_round_up(
            slot_hdr_off + (size_t)total_slots * sizeof(mt_xs_slot_hdr_t),
            PERL_KRT_CACHE_LINE);
        size_t *req_off, *reply_off, *arena_off;
        size_t vec_off;
        uint32_t total_vecs = (uint32_t)ncomp + total_slots + (uint32_t)nspares;
        off = spare_hdr_off + (size_t)nspares * sizeof(mt_xs_spare_hdr_t);
        off = S_round_up(off, PERL_KRT_CACHE_LINE);
        vec_off = off;
        off += perl_krt_vectab_size(total_vecs);
        Newx(req_off, ncomp, size_t);
        Newx(reply_off, total_slots, size_t);
        Newx(arena_off, total_slots, size_t);
        for (i = 0; i < (uint32_t)ncomp; i++) {
            off = S_round_up(off, PERL_KRT_CACHE_LINE);
            req_off[i] = off;
            off += perl_krt_mpsc_bytes(MT_XS_REQ_RING_CAP, sizeof(mt_xs_frame_t));
        }
        for (i = 0; i < total_slots; i++) {
            off = S_round_up(off, PERL_KRT_CACHE_LINE);
            reply_off[i] = off;
            off += perl_krt_spsc_bytes(MT_XS_REPLY_RING_CAP, sizeof(mt_xs_frame_t));
            off = S_round_up(off, page);
            arena_off[i] = off;
            off += S_pool.arena_len;
        }
        total = S_round_up(off, page);

        if (perl_krt_shm_create(&S_pool.shm, total, 0) != 0) {
            Safefree(req_off); Safefree(reply_off); Safefree(arena_off);
            Zero(&S_pool.shm, 1, perl_krt_shm_t);
            return;                     /* no compartments on this platform */
        }
        S_pool.hdr = (mt_xs_shm_hdr_t *)S_pool.shm.base;
        S_pool.hdr->magic = MT_XS_SHM_MAGIC;
        S_pool.hdr->ncomp = (uint32_t)ncomp;
        S_pool.hdr->nslots = total_slots;
        S_pool.hdr->nspares = (uint32_t)nspares;
        S_pool.hdr->comp_hdr_off = comp_hdr_off;
        S_pool.hdr->slot_hdr_off = slot_hdr_off;
        S_pool.hdr->spare_hdr_off = spare_hdr_off;
        S_pool.hdr->vec_off = vec_off;
        S_pool.hdr->total = total;
        S_pool.vec = perl_krt_vectab_init(MT_XS_AT(vec_off),
                                          total - vec_off,
                                          total_vecs);
        if (!S_pool.vec) {
            Safefree(req_off); Safefree(reply_off); Safefree(arena_off);
            perl_krt_shm_destroy(&S_pool.shm);
            Zero(&S_pool.shm, 1, perl_krt_shm_t);
            return;
        }
        S_pool.comps = (mt_xs_comp_hdr_t *)MT_XS_AT(comp_hdr_off);
        S_pool.slots = (mt_xs_slot_hdr_t *)MT_XS_AT(slot_hdr_off);
        S_pool.spares = nspares > 0 ? (mt_xs_spare_hdr_t *)MT_XS_AT(spare_hdr_off) : NULL;
        S_pool.ncomp = (uint32_t)ncomp;
        S_pool.nslots = total_slots;
        S_pool.worker_slots = (uint32_t)nslots;
        S_pool.reload_slot = nspares > 0 ? (uint32_t)nslots : UINT32_MAX;
        S_pool.nspares = (uint32_t)nspares;
        for (i = 0; i < S_pool.ncomp; i++) {
            S_pool.comps[i].req_ring_off = req_off[i];
            atomic_init(&S_pool.comps[i].gen, 1);
            atomic_init(&S_pool.comps[i].alive, 0);
            atomic_init(&S_pool.comps[i].pid, 0);
            atomic_init(&S_pool.comps[i].calls, 0);
            (void)perl_krt_mpsc_init_flags(MT_XS_AT(req_off[i]),
                                     perl_krt_mpsc_bytes(MT_XS_REQ_RING_CAP,
                                                         sizeof(mt_xs_frame_t)),
                                     MT_XS_REQ_RING_CAP, sizeof(mt_xs_frame_t),
                                     PERL_KRT_RING_SHARED);
            S_pool.hc[i].gen = 1;
        }
        for (i = 0; i < S_pool.nslots; i++) {
            S_pool.slots[i].reply_ring_off = reply_off[i];
            S_pool.slots[i].arena_off = arena_off[i];
            S_pool.slots[i].arena_len = S_pool.arena_len;
            atomic_init(&S_pool.slots[i].in_use,
                        (i == S_pool.reload_slot) ? 1u : 0u);
            atomic_init(&S_pool.slots[i].worker_id, 0);
            atomic_init(&S_pool.slots[i].comp, 0);
            atomic_init(&S_pool.slots[i].bump, 0);
            atomic_init(&S_pool.slots[i].in_call, 0);
            (void)perl_krt_spsc_init_flags(MT_XS_AT(reply_off[i]),
                                     perl_krt_spsc_bytes(MT_XS_REPLY_RING_CAP,
                                                         sizeof(mt_xs_frame_t)),
                                     MT_XS_REPLY_RING_CAP, sizeof(mt_xs_frame_t),
                                     PERL_KRT_RING_SHARED);
        }
        for (i = 0; i < S_pool.nspares; i++) {
            atomic_init(&S_pool.spares[i].assigned_comp, UINT32_MAX);
            atomic_init(&S_pool.spares[i].pid, 0u);
            atomic_init(&S_pool.spares[i].ready, 0u);
            atomic_init(&S_pool.spares[i].shutdown, 0u);
        }
        Safefree(req_off); Safefree(reply_off); Safefree(arena_off);
    }

    /* the children: nothing buffered may be flushed twice */
    PerlIO_flush((PerlIO *)NULL);
    S_pool.parent_pid = getpid();
    for (i = 0; i < S_pool.ncomp; i++) {
        static uint32_t S_child_arg[MT_XS_MAX_COMPARTMENTS];
        S_child_arg[i] = i;
        S_pool.comp_cpu[i] = -1;
        if (perl_krt_comp_spawn(&S_pool.hc[i].proc, S_mt_xs_comp_main,
                                &S_child_arg[i], &S_pool.shm, 1) != 0) {
            S_pool.hc[i].dead = 1;
            atomic_fetch_add_explicit(&S_pool.comps[i].gen, 1u,
                                      memory_order_acq_rel);
            S_pool.hc[i].gen++;
        }
    }
    /* pre-forked spares: forked from the single-threaded pre-image state */
    for (i = 0; i < S_pool.nspares; i++) {
        static uint32_t S_spare_arg[MT_XS_MAX_SPARES];
        S_spare_arg[i] = i;
        S_pool.hsp[i].used = 0;
        S_pool.hsp[i].dead = 0;
        if (perl_krt_comp_spawn(&S_pool.hsp[i].proc, S_mt_xs_spare_main,
                                &S_spare_arg[i], &S_pool.shm, 1) != 0) {
            S_pool.hsp[i].dead = 1;
        }
    }
    /* PERL_MT_XS_AFFINITY=smt: compartment i sits on CPU i and the workers
     * bound to it are pinned to its SMT siblings (Linux; elsewhere the
     * lists stay empty and nothing is pinned) */
    if (S_pool.affinity) {
        for (i = 0; i < S_pool.ncomp; i++) {
            int cpu = (int)i, n;
            if (S_pool.hc[i].dead)
                continue;
            n = perl_krt_smt_siblings(cpu, S_pool.sib[i], MT_XS_MAX_SIBLINGS);
            if (n <= 1)
                continue;                     /* no SMT pair: no pinning */
            S_pool.nsib[i] = n;
            S_pool.comp_cpu[i] = cpu;
        }
    }
    /* the reapers: one per live compartment, after every fork */
    for (i = 0; i < S_pool.ncomp; i++) {
        if (S_pool.hc[i].dead)
            continue;
        if (pthread_create(&S_pool.reaper[i], NULL, S_mt_xs_reaper,
                           (void *)(uintptr_t)i) != 0) {
            /* no reaper: deaths are noticed at the backstop timeout */
            S_pool.reaper[i] = (pthread_t)0;
        }
    }
}

void
Perl_mt_xs_pool_stop(pTHX)
{
    uint32_t i;
    PERL_UNUSED_CONTEXT;
    if (!S_pool.started || S_pool.child || !S_pool.shm.base)
        return;
    atomic_store_explicit(&S_pool.stopping, 1u, memory_order_release);
    for (i = 0; i < S_pool.ncomp; i++) {
        if (!S_pool.hc[i].dead && S_pool.hc[i].proc.pid > 0) {
            int st;
            (void)perl_krt_comp_kill(&S_pool.hc[i].proc, SIGTERM);
            (void)perl_krt_comp_reap(&S_pool.hc[i].proc, &st);
            S_pool.hc[i].dead = 1;
        }
    }
    for (i = 0; i < S_pool.nspares; i++) {
        if (!S_pool.hsp[i].used && !S_pool.hsp[i].dead && S_pool.hsp[i].proc.pid > 0) {
            int st;
            atomic_store_explicit(&S_pool.spares[i].shutdown, 1u, memory_order_release);
            (void)perl_krt_vec_post(S_pool.vec, S_vec_spare(i));
            (void)perl_krt_comp_kill(&S_pool.hsp[i].proc, SIGTERM);
            (void)perl_krt_comp_reap(&S_pool.hsp[i].proc, &st);
            S_pool.hsp[i].dead = 1;
        }
    }
}

/* ---- host: compartment liveness, restart and slots ----------------------- */

/* Caller must hold S_pool.mutex. */
static void
S_mt_xs_mark_dead_locked(uint32_t comp, int wait_status)
{
    mt_xs_host_comp_t * const hc = &S_pool.hc[comp];
    uint32_t old_gen = hc->gen;
    uint32_t s;
    uint64_t inflight = 0;
    char msg[MT_FAULT_MSG_MAX];

    atomic_store_explicit(&S_pool.comps[comp].alive, 0u, memory_order_release);
    hc->dead = 1;
    hc->gen++;
    atomic_fetch_add_explicit(&S_pool.comps[comp].gen, 1u, memory_order_acq_rel);

    for (s = 0; s < S_pool.worker_slots; s++) {
        if (atomic_load_explicit(&S_pool.slots[s].in_use, memory_order_acquire)
            && atomic_load_explicit(&S_pool.slots[s].comp, memory_order_acquire) == comp
            && atomic_load_explicit(&S_pool.slots[s].in_call, memory_order_acquire) > 0)
            inflight++;
    }
    hc->failed_inflight += inflight;

    if (WIFSIGNALED(wait_status))
        (void)snprintf(msg, sizeof msg,
                       "XS compartment %u (pid %ld, gen %u) killed by signal %d",
                       (unsigned)comp, (long)hc->proc.pid, (unsigned)old_gen,
                       WTERMSIG(wait_status));
    else if (WIFEXITED(wait_status))
        (void)snprintf(msg, sizeof msg,
                       "XS compartment %u (pid %ld, gen %u) exited with status %d",
                       (unsigned)comp, (long)hc->proc.pid, (unsigned)old_gen,
                       WEXITSTATUS(wait_status));
    else
        (void)snprintf(msg, sizeof msg,
                       "XS compartment %u (pid %ld, gen %u) died",
                       (unsigned)comp, (long)hc->proc.pid, (unsigned)old_gen);

    Perl_mt_fault_record(MT_FAULT_COMPARTMENT_CRASH, MT_FAULT_RECOVERABLE,
                         MT_UNIT_COMPARTMENT, comp, old_gen,
                         MT_COMPARTMENT_DEAD, msg);
    S_mt_xs_wake_comp(comp);
}

/* Reload all previously recorded LOAD payloads into a promoted compartment
 * using the reserved reload_slot (no PerlInterpreter required). */
static void
S_mt_xs_reload_comp_modules(uint32_t comp, mt_xs_loaded_mod_t *mods)
{
    const uint32_t slot = S_pool.reload_slot;
    perl_krt_mpsc_t *req_ring;
    perl_krt_spsc_t *rep_ring;
    mt_xs_loaded_mod_t *m;
    uint32_t seq = 1;

    if (slot == UINT32_MAX || !mods)
        return;

    MUTEX_LOCK(&S_pool.reload_mutex);
    req_ring = S_req_ring(comp);
    rep_ring = S_reply_ring(slot);
    /* Drain any leftover replies from an earlier reload */
    {
        mt_xs_frame_t junk;
        while (perl_krt_spsc_pop(rep_ring, &junk) == 0)
            ;
    }
    atomic_store_explicit(&S_pool.slots[slot].comp, comp, memory_order_release);

    for (m = mods; m; m = m->next) {
        mt_xs_frame_t f, r;
        int got = 0;
        uint64_t deadline;

        if (m->payload_len > S_pool.arena_len)
            continue;
        if (!atomic_load_explicit(&S_pool.comps[comp].alive, memory_order_acquire))
            break;

        memcpy(S_arena(slot), m->payload, m->payload_len);
        atomic_store_explicit(&S_pool.slots[slot].bump, (uint32_t)m->payload_len,
                              memory_order_release);
        atomic_store_explicit(&S_pool.slots[slot].in_call, 1u, memory_order_release);

        memset(&f, 0, sizeof f);
        f.type  = MT_XSF_LOAD;
        f.slot  = slot;
        f.seq   = seq++;
        f.flags = m->flags;
        f.off   = 0;
        f.len   = m->payload_len;

        while (perl_krt_mpsc_push(req_ring, &f, sizeof f) != 0) {
            if (!atomic_load_explicit(&S_pool.comps[comp].alive, memory_order_acquire))
                break;
            S_mt_xs_pause();
        }
        (void)perl_krt_vec_post(S_pool.vec, S_vec_comp(comp));

        deadline = perl_krt_now_ns() + 5000000000ull; /* 5s reload timeout */
        while (perl_krt_now_ns() < deadline) {
            const uint32_t d = perl_krt_vec_doorbell(S_pool.vec, S_vec_slot(slot));
            while (perl_krt_spsc_pop(rep_ring, &r) == 0) {
                if ((r.type == MT_XSF_RET || r.type == MT_XSF_DIE) && r.seq == f.seq) {
                    got = 1;
                    break;
                }
            }
            if (got)
                break;
            if (!atomic_load_explicit(&S_pool.comps[comp].alive, memory_order_acquire))
                break;
            (void)perl_krt_vec_wait(S_pool.vec, S_vec_slot(slot), d, NULL,
                                    50000000ull);
        }
        atomic_store_explicit(&S_pool.slots[slot].in_call, 0u, memory_order_release);
        if (!got)
            break;
    }

    MUTEX_UNLOCK(&S_pool.reload_mutex);
}

/* Promote a pre-forked spare compartment to take over `comp`.
 * Returns 1 if a spare took over, 0 if no spares remain. */
static int
S_mt_xs_promote_spare(uint32_t comp)
{
    uint32_t sp = UINT32_MAX, i;
    uint64_t t0, deadline;
    mt_xs_loaded_mod_t *mods = NULL;

    if (atomic_load_explicit(&S_pool.stopping, memory_order_acquire))
        return 0;

    t0 = perl_krt_now_ns();

    MUTEX_LOCK(&S_pool.mutex);
    for (i = 0; i < S_pool.nspares; i++) {
        if (!S_pool.hsp[i].used && !S_pool.hsp[i].dead && S_pool.hsp[i].proc.pid > 0) {
            sp = i;
            S_pool.hsp[i].used = 1;
            break;
        }
    }
    if (sp == UINT32_MAX) {
        MUTEX_UNLOCK(&S_pool.mutex);
        return 0;
    }
    mods = S_pool.hc[comp].loaded_mods;
    MUTEX_UNLOCK(&S_pool.mutex);

    /* Wait for the spare's Perl interpreter to finish startup if needed */
    deadline = perl_krt_now_ns() + 5000000000ull;
    while (!atomic_load_explicit(&S_pool.spares[sp].ready, memory_order_acquire)
           && perl_krt_now_ns() < deadline) {
        if (atomic_load_explicit(&S_pool.stopping, memory_order_acquire))
            return 0;
        S_mt_xs_pause();
    }

    /* Reset the dead compartment's request ring before promoting the spare */
    MUTEX_LOCK(&S_pool.mutex);
    (void)perl_krt_mpsc_init_flags(S_req_ring(comp),
                                   perl_krt_mpsc_bytes(MT_XS_REQ_RING_CAP,
                                                       sizeof(mt_xs_frame_t)),
                                   MT_XS_REQ_RING_CAP, sizeof(mt_xs_frame_t),
                                   PERL_KRT_RING_SHARED);
    S_pool.hc[comp].proc = S_pool.hsp[sp].proc;
    atomic_store_explicit(&S_pool.spares[sp].assigned_comp, comp,
                          memory_order_release);
    (void)perl_krt_vec_post(S_pool.vec, S_vec_spare(sp));
    MUTEX_UNLOCK(&S_pool.mutex);

    /* Wait for the promoted spare to claim S_vec_comp(comp) and mark alive */
    deadline = perl_krt_now_ns() + 5000000000ull;
    while (!atomic_load_explicit(&S_pool.comps[comp].alive, memory_order_acquire)
           && perl_krt_now_ns() < deadline) {
        if (atomic_load_explicit(&S_pool.stopping, memory_order_acquire))
            return 0;
        S_mt_xs_pause();
    }
    if (!atomic_load_explicit(&S_pool.comps[comp].alive, memory_order_acquire))
        return 0;

    /* Reload every module the dead compartment had served */
    if (mods)
        S_mt_xs_reload_comp_modules(comp, mods);

    MUTEX_LOCK(&S_pool.mutex);
    S_pool.hc[comp].dead = 0;
    S_pool.hc[comp].restarts++;
    S_pool.hc[comp].last_restart_ns = perl_krt_now_ns() - t0;
    S_mt_xs_wake_comp(comp);
    MUTEX_UNLOCK(&S_pool.mutex);
    return 1;
}

static void
S_mt_xs_record_loaded_mod(uint32_t comp, const char *mod, size_t mlen,
                          uint32_t flags, const void *payload, size_t plen)
{
    mt_xs_loaded_mod_t *m, **tail;
    if (comp >= S_pool.ncomp || !mod || !payload || !plen)
        return;
    MUTEX_LOCK(&S_pool.mutex);
    tail = &S_pool.hc[comp].loaded_mods;
    while (*tail) {
        if (strlen((*tail)->module) == mlen
            && memcmp((*tail)->module, mod, mlen) == 0
            && (*tail)->flags == flags) {
            MUTEX_UNLOCK(&S_pool.mutex);
            return;
        }
        tail = &(*tail)->next;
    }
    m = (mt_xs_loaded_mod_t *)calloc(1, sizeof *m);
    if (m) {
        m->module = (char *)malloc(mlen + 1);
        m->payload = malloc(plen);
        if (m->module && m->payload) {
            memcpy(m->module, mod, mlen);
            m->module[mlen] = '\0';
            memcpy(m->payload, payload, plen);
            m->payload_len = plen;
            m->flags = flags;
            *tail = m;
            S_pool.hc[comp].nloaded++;
        }
        else {
            free(m->module);
            free(m->payload);
            free(m);
        }
    }
    MUTEX_UNLOCK(&S_pool.mutex);
}

/* Is compartment `comp` (at generation `gen`) still there?  A dead one
 * has its generation bumped so that every handle and stub into it is
 * stale from then on.  Any worker may ask; the reap is serialised. */
static bool
S_mt_xs_comp_check(uint32_t comp)
{
    mt_xs_host_comp_t *hc;
    int st = 0, rc;
    if (comp >= S_pool.ncomp)
        return FALSE;
    hc = &S_pool.hc[comp];
    if (hc->dead)
        return FALSE;
    MUTEX_LOCK(&S_pool.mutex);
    if (!hc->dead) {
        rc = perl_krt_comp_reap_flags(&hc->proc, &st, PERL_KRT_REAP_NOHANG);
        if (rc != -EAGAIN) {
            /* exited (0), or already reaped by someone else (-ECHILD) */
            S_mt_xs_mark_dead_locked(comp, st);
        }
    }
    MUTEX_UNLOCK(&S_pool.mutex);
    return !hc->dead;
}

/* A compartment died: every worker slot bound to it is woken, so that its
 * parked waiters read the death at once (the reaper calls this too). */
static void
S_mt_xs_wake_comp(uint32_t comp)
{
    uint32_t s;
    if (!S_pool.vec)
        return;
    for (s = 0; s < S_pool.nslots; s++) {
        if (atomic_load_explicit(&S_pool.slots[s].in_use, memory_order_acquire)
            && atomic_load_explicit(&S_pool.slots[s].comp, memory_order_acquire)
               == comp)
            (void)perl_krt_vec_post(S_pool.vec, S_vec_slot(s));
    }
}

/*
=for apidoc mt_xs_comp_alive

Whether compartment C<comp> is alive and still at generation C<gen>.

=for apidoc mt_xs_comp_count

The number of compartments in the pool (zero before the freeze or with
C<PERL_MT_XS_COMPARTMENTS=0>).

=for apidoc mt_xs_bind_comp

The compartment this interpreter sends its legacy XS to, chosen on first
use (workers are spread round robin over the live compartments) and kept
for the worker's life so that every handle it holds has one home.  Dies
with C<MT_NO_COMPARTMENT> when the pool is empty or every compartment is
dead.

=cut
*/

bool
Perl_mt_xs_comp_alive(pTHX_ U32 comp, U32 gen)
{
    PERL_UNUSED_CONTEXT;
    if (!S_pool.shm.base || comp >= S_pool.ncomp)
        return FALSE;
    if (S_pool.hc[comp].gen != gen)
        return FALSE;
    return S_mt_xs_comp_check(comp);
}

U32
Perl_mt_xs_comp_count(pTHX)
{
    PERL_UNUSED_CONTEXT;
    return S_pool.shm.base ? S_pool.ncomp : 0;
}

/* PERL_MT_XS_AFFINITY=smt: a worker bound to compartment c pins its
 * thread to the next SMT sibling of c's CPU that is not c's own (round
 * robin over the workers bound to c; none left, no pin).  Linux only:
 * elsewhere the sibling lists are empty.  Once per worker. */
static void
S_mt_xs_pin_worker(pTHX)
{
    const I32 c = PL_mt_xs_comp;
    uint32_t k, seen = 0;
    int i;
    if (!S_pool.affinity || c < 0 || (U32)c >= S_pool.ncomp
        || S_pool.nsib[c] <= 0 || PL_mt_xs_pinned != -1)
        return;                           /* -1: not decided yet */
    PL_mt_xs_pinned = -2;                 /* decided: no pin unless found */
    MUTEX_LOCK(&S_pool.mutex);
    k = S_pool.sib_next[c]++;
    MUTEX_UNLOCK(&S_pool.mutex);
    for (i = 0; i < S_pool.nsib[c]; i++) {
        const int cpu = S_pool.sib[c][i];
        if (cpu == S_pool.comp_cpu[c])
            continue;
        if (seen++ == k) {
            if (perl_krt_cpu_pin(cpu) == 0)
                PL_mt_xs_pinned = (I32)cpu;
            return;
        }
    }
}

I32
Perl_mt_xs_bind_comp(pTHX)
{
    uint32_t i, n;
    if (PL_mt_xs_comp >= 0 && (U32)PL_mt_xs_comp < S_pool.ncomp
        && S_mt_xs_comp_check((uint32_t)PL_mt_xs_comp)) {
        S_mt_xs_pin_worker(aTHX);
        return PL_mt_xs_comp;
    }
    if (!S_pool.shm.base || !S_pool.ncomp)
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    MUTEX_LOCK(&S_pool.mutex);
    n = S_pool.next_comp++;
    MUTEX_UNLOCK(&S_pool.mutex);
    for (i = 0; i < S_pool.ncomp; i++) {
        const uint32_t c = (n + i) % S_pool.ncomp;
        if (S_mt_xs_comp_check(c)) {
            PL_mt_xs_comp = (I32)c;
            S_mt_xs_pin_worker(aTHX);
            return PL_mt_xs_comp;
        }
    }
    Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
}

/*
=for apidoc mt_xs_slot

This interpreter's bridge slot (a reply ring and an arena in the shared
segment), acquired on first use.  Dies with C<MT_NO_COMPARTMENT> when all
C<PERL_MT_XS_SLOTS> are taken.

=for apidoc mt_xs_slot_release

Gives the slot back (at worker exit).

=cut
*/

U32
Perl_mt_xs_slot(pTHX)
{
    uint32_t i;
    if (PL_mt_xs_slot >= 0)
        return (U32)PL_mt_xs_slot;
    if (!S_pool.shm.base)
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    for (i = 0; i < S_pool.worker_slots; i++) {
        uint32_t zero = 0;
        if (perl_krt_cas_strong_acq_rel(&S_pool.slots[i].in_use, &zero, 1u)) {
            atomic_store_explicit(&S_pool.slots[i].worker_id, PL_mt_worker_id,
                                  memory_order_relaxed);
            atomic_store_explicit(&S_pool.slots[i].bump, 0u,
                                  memory_order_relaxed);
            S_pool.slot_seq[i] = 1;
            PL_mt_xs_slot = (I32)i;
            return i;
        }
    }
    Perl_croak(aTHX_ "perl5-mt: no free XS bridge slot (PERL_MT_XS_SLOTS) "
                     "(MT_NO_COMPARTMENT)");
}

void
Perl_mt_xs_slot_release(pTHX)
{
    Perl_mt_xs_host_handles_free(aTHX);
    if (PL_mt_xs_slot < 0 || !S_pool.shm.base)
        return;
    {
        const uint32_t s = (uint32_t)PL_mt_xs_slot;
        mt_xs_frame_t f;
        /* drain whatever the compartment still had for us */
        while (perl_krt_spsc_pop(S_reply_ring(s), &f) == 0)
            ;
        atomic_store_explicit(&S_pool.slots[s].worker_id, 0u,
                              memory_order_relaxed);
        atomic_store_explicit(&S_pool.slots[s].in_use, 0u,
                              memory_order_release);
    }
    PL_mt_xs_slot = -1;
}

/* ---- host: frames -------------------------------------------------------- */

/*
=for apidoc mt_xs_arena_alloc

Reserves C<len> bytes of the slot's arena for a frame payload and returns
its address, with the offset (what the frame carries) in C<*off>.  The
arena is a bump allocator that both ends of the bridge advance in turn
while one outermost call is in flight; L</mt_xs_arena_reset> rewinds it.
Dies with C<MT_REMOTE_FRAME_TOO_LARGE>.

=cut
*/

void *
Perl_mt_xs_arena_alloc(pTHX_ U32 slot, size_t len, U64 *off)
{
    mt_xs_slot_hdr_t * const sh = &S_pool.slots[slot];
    const uint32_t want = (uint32_t)S_round_up(len, 16);
    uint32_t cur;
    PERL_ARGS_ASSERT_MT_XS_ARENA_ALLOC;
    /* host and compartment both allocate on this slot (a fiber posts while
     * the compartment is writing a reply): the reservation is one CAS, so
     * two allocations never get the same bytes */
    if (len > S_pool.arena_len)
        Perl_croak_mt(aTHX_ MT_REMOTE_FRAME_TOO_LARGE);
    cur = atomic_load_explicit(&sh->bump, memory_order_acquire);
    do {
        if ((size_t)cur + want > S_pool.arena_len)
            Perl_croak_mt(aTHX_ MT_REMOTE_FRAME_TOO_LARGE);
    } while (!perl_krt_cas_strong_acq_rel(&sh->bump, &cur, cur + want));
    *off = cur;
    return S_arena(slot) + cur;
}

void
Perl_mt_xs_arena_reset(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    atomic_store_explicit(&S_pool.slots[slot].bump, 0u, memory_order_release);
}

void *
Perl_mt_xs_arena_at(pTHX_ U32 slot, U64 off, U64 len)
{
    PERL_UNUSED_CONTEXT;
    if (off > S_pool.arena_len || len > S_pool.arena_len - off)
        Perl_croak_mt(aTHX_ MT_REMOTE_FRAME_TOO_LARGE);
    return S_arena(slot) + off;
}

/*
=for apidoc mt_xs_post

Posts frame C<f> to compartment C<comp>'s request ring, waiting while the
ring is full; dies with C<MT_COMPARTMENT_DEAD> when the compartment is
gone.  Used by the host for requests and for callback replies.

=cut
*/

void
Perl_mt_xs_post(pTHX_ U32 comp, const mt_xs_frame_t *f)
{
    perl_krt_mpsc_t *ring;
    PERL_ARGS_ASSERT_MT_XS_POST;
    if (comp >= S_pool.ncomp || S_pool.hc[comp].dead)
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    if (f->slot < S_pool.nslots)
        atomic_store_explicit(&S_pool.slots[f->slot].comp, comp, memory_order_release);
    ring = S_req_ring(comp);
    /* a frame without a worker slot (MT_XSF_CRASH) has no reply vector */
    for (;;) {
        const int has_slot = f->slot < S_pool.nslots;
        const uint32_t d = has_slot
            ? perl_krt_vec_doorbell(S_pool.vec, S_vec_slot(f->slot)) : 0;
        if (perl_krt_mpsc_push(ring, f, sizeof *f) == 0)
            break;
        /* full: the compartment is behind (or dead).  Its replies to this
         * slot ring the slot vector; the budget covers the rest. */
        if (!S_mt_xs_comp_check(comp))
            Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
        if (has_slot)
            (void)perl_krt_vec_wait(S_pool.vec, S_vec_slot(f->slot), d, NULL,
                                    200000ull);
        else
            S_mt_xs_pause();
    }
    (void)perl_krt_vec_post(S_pool.vec, S_vec_comp(comp));
}

/*
=for apidoc mt_xs_wait

Blocks until compartment C<comp> answers the call C<seq> of slot C<slot>
with C<RET> or C<DIE> (copied to C<*out>).  Frames that arrive meanwhile
are served here: a C<CALLBACK> runs a host code ref (and may recurse into
further remote calls), an C<HREL> releases a host code handle.  The
doorbell of the slot's reply vector is the signal, so a reply wakes the
waiter at once; a compartment's death wakes it through the reaper, and the
C<PERL_MT_XS_...> backstop of one second is only a timeout.

Two modes.  A scheduled fiber (the loop of this interpreter is running and
the fiber is one of its scheduled fibers) parks instead of blocking: it is
put on C<PL_mt_xs_waiters>, the loop's idle hook (C<Perl_mt_sched_idle>)
drains the slot's reply ring, stores each reply on its fiber and wakes it.
Any other context blocks its thread on the slot vector with the
interpreter's adaptive policy.  Both modes deliver the replies of the
other waiters of the slot that they pop on the way.

=cut
*/

/* Is the current context a scheduled fiber whose loop is running?  Its
 * remote calls park rather than block. */
#define S_xs_parkable(aTHX)  MT_FIBER_PARKABLE(PL_mt_fiber_current, PL_mt_sched_fiber)

/* Handles one frame popped from slot `comp`'s reply ring.  Returns TRUE
 * when the frame is the reply the caller (a thread) waits for: it is
 * copied to *out.  Replies to parked fibers are stored on the fiber and
 * the fiber is woken (the count of woken fibers goes to *woke).  A
 * callback is served here; the other frames are dropped as before. */
static bool
S_xs_frame(pTHX_ U32 comp, const mt_xs_frame_t *f, U32 want,
           mt_xs_frame_t *out, int *woke)
{
    switch (f->type) {
    case MT_XSF_RET:
    case MT_XSF_DIE: {
        struct mt_fiber *w, *prev = NULL;
        if (f->seq == want) {
            *out = *f;
            return TRUE;
        }
        for (w = PL_mt_xs_waiters; w; prev = w, w = w->next_xs_waiter) {
            if (w->xs_wait_seq == f->seq && w->xs_wait_slot == f->slot)
                break;
        }
        if (!w)
            return FALSE;        /* a reply to a call that is no longer waited for */
        if (prev)
            prev->next_xs_waiter = w->next_xs_waiter;
        else
            PL_mt_xs_waiters = w->next_xs_waiter;
        w->next_xs_waiter = NULL;
        /* the payload stays in the arena only until the next call on the slot
         * reuses it: a fiber that runs later decodes its own copy */
        w->xs_reply = *f;
        if (f->len) {
            char * const copy = (char *)safemalloc((size_t)f->len);
            Copy(Perl_mt_xs_arena_at(aTHX_ f->slot, f->off, f->len), copy,
                 (size_t)f->len, char);
            w->xs_reply.flags |= MT_XSF_FLAG_COPIED;
            w->xs_reply.pad[0] = PTR2UV(copy);
        }
        w->xs_wait_flags |= MT_FIBER_XS_DONE;
        Perl_mt_sched_wake(aTHX_ w);
        if (woke)
            (*woke)++;
        return FALSE;
    }
    case MT_XSF_CALLBACK:
        Perl_mt_xs_host_callback(aTHX_ comp, f);
        return FALSE;
    case MT_XSF_HREL:
        Perl_mt_xs_host_hrel(aTHX_ f);
        return FALSE;
    default:
        return FALSE;
    }
}

/* Parks the current scheduled fiber on call `seq`: it is linked on the
 * waiters and resumes with the reply stored by the drain, or with the
 * compartment's death. */
static void
S_xs_park(pTHX_ U32 comp, U32 slot, U32 seq, mt_xs_frame_t *out)
{
    struct mt_fiber * const f = PL_mt_fiber_current;
    f->xs_wait_seq = seq;
    f->xs_wait_slot = slot;
    f->xs_wait_comp = comp;
    f->xs_wait_flags = 0;
    f->next_xs_waiter = PL_mt_xs_waiters;
    PL_mt_xs_waiters = f;
    Perl_mt_sched_arm(aTHX);
    /* resumed by the drain, or by an outside wake: only the drain (a reply
     * or the death) ends the wait, so anything else parks again */
    do {
        Perl_mt_sched_park(aTHX);
    } while (!(f->xs_wait_flags & (MT_FIBER_XS_DONE | MT_FIBER_XS_DEAD)));
    if (!(f->xs_wait_flags & MT_FIBER_XS_DONE)) {
        f->xs_wait_flags = 0;
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    }
    *out = f->xs_reply;
    f->xs_wait_flags = 0;
}

/* Wakes the parked fibers whose compartment is dead, with the death
 * (they unlink).  Returns how many were woken. */
static int
S_mt_xs_wake_dead(pTHX)
{
    struct mt_fiber *w = PL_mt_xs_waiters;
    int woke = 0;
    while (w) {
        struct mt_fiber *next = w->next_xs_waiter;
        if (!S_mt_xs_comp_check(w->xs_wait_comp)) {
            struct mt_fiber *p = NULL, *q;
            for (q = PL_mt_xs_waiters; q && q != w; p = q, q = q->next_xs_waiter)
                ;
            if (p)
                p->next_xs_waiter = w->next_xs_waiter;
            else
                PL_mt_xs_waiters = w->next_xs_waiter;
            w->next_xs_waiter = NULL;
            w->xs_wait_flags = MT_FIBER_XS_DEAD;
            Perl_mt_sched_wake(aTHX_ w);
            woke++;
        }
        w = next;
    }
    return woke;
}

/* The non-blocking drain of the XS waiters (the scheduler's idle
 * hook and its poll both call it, see mt_fiber.c).  It serves the replies
 * that arrived on the reply ring of the waiters' slot, and the deaths the
 * reaper has seen.  Never blocks.  *woke, when not NULL, gets the number of
 * fibers it woke. */
void
Perl_mt_xs_drain(pTHX_ int *woke)
{
    perl_krt_spsc_t *ring;
    struct mt_fiber *w;
    mt_xs_frame_t f;
    int n = 0;
    U32 comp, slot;
    if (!PL_mt_xs_waiters)
        return;
    /* taken once: a reply in this batch may unlink the last waiter */
    comp = PL_mt_xs_waiters->xs_wait_comp;
    slot = PL_mt_xs_waiters->xs_wait_slot;
    ring = S_reply_ring(slot);
    while (perl_krt_spsc_pop(ring, &f) == 0)
        (void)S_xs_frame(aTHX_ comp, &f, UINT32_MAX, NULL, &n);
    for (w = PL_mt_xs_waiters; w; w = w->next_xs_waiter) {
        if (!atomic_load_explicit(&S_pool.comps[w->xs_wait_comp].alive,
                                  memory_order_acquire)) {
            n += S_mt_xs_wake_dead(aTHX);
            break;
        }
    }
    if (woke)
        *woke += n;
}

/* the doorbell of this worker's reply vector, for an idle loop
 * that waits on it.  Returns 1 with *doorbell set when XS waiters exist, 0
 * when none does. */
int
Perl_mt_xs_bell(pTHX_ U32 *doorbell)
{
    PERL_ARGS_ASSERT_MT_XS_BELL;
    if (!PL_mt_xs_waiters)
        return 0;
    *doorbell = perl_krt_vec_doorbell(S_pool.vec,
                    S_vec_slot(PL_mt_xs_waiters->xs_wait_slot));
    return 1;
}

/* has the doorbell read by Perl_mt_xs_bell moved on? */
int
Perl_mt_xs_bell_changed(pTHX_ U32 doorbell)
{
    if (!PL_mt_xs_waiters)
        return 1;
    return perl_krt_vec_doorbell(S_pool.vec,
               S_vec_slot(PL_mt_xs_waiters->xs_wait_slot)) != doorbell;
}

/* block on the reply vector while its doorbell still reads
 * `doorbell`, for at most timeout_ns.  The caller drains again afterwards. */
void
Perl_mt_xs_bell_wait(pTHX_ U32 doorbell, uint64_t timeout_ns)
{
    if (!PL_mt_xs_waiters)
        return;
    if (!PL_mt_xs_poll.min_ns && !PL_mt_xs_poll.max_ns)
        perl_krt_poll_policy_init(&PL_mt_xs_poll, PERL_KRT_POLL_DEFAULT_MIN_NS,
                                  PERL_KRT_POLL_DEFAULT_MAX_NS);
    (void)perl_krt_vec_wait(S_pool.vec,
                            S_vec_slot(PL_mt_xs_waiters->xs_wait_slot),
                            doorbell, &PL_mt_xs_poll, timeout_ns);
}

void
Perl_mt_xs_wait(pTHX_ U32 comp, U32 slot, U32 seq, mt_xs_frame_t *out)
{
    perl_krt_spsc_t * const ring = S_reply_ring(slot);
    const uint32_t start_gen = atomic_load_explicit(&S_pool.comps[comp].gen,
                                                    memory_order_acquire);
    PERL_ARGS_ASSERT_MT_XS_WAIT;
    if (S_xs_parkable(aTHX)) {
        S_xs_park(aTHX_ comp, slot, seq, out);
        return;
    }
    if (!PL_mt_xs_poll.min_ns && !PL_mt_xs_poll.max_ns)
        perl_krt_poll_policy_init(&PL_mt_xs_poll, PERL_KRT_POLL_DEFAULT_MIN_NS,
                                  PERL_KRT_POLL_DEFAULT_MAX_NS);
    for (;;) {
        const uint32_t d = perl_krt_vec_doorbell(S_pool.vec, S_vec_slot(slot));
        mt_xs_frame_t f;
        int woke = 0;
        while (perl_krt_spsc_pop(ring, &f) == 0) {
            if (S_xs_frame(aTHX_ comp, &f, seq, out, &woke))
                return;
        }
        (void)perl_krt_vec_wait(S_pool.vec, S_vec_slot(slot), d, &PL_mt_xs_poll,
                                MT_XS_WAIT_BACKSTOP_NS);
        if (perl_krt_spsc_count(ring) == 0
            && (atomic_load_explicit(&S_pool.comps[comp].gen,
                                     memory_order_acquire) != start_gen
                || !S_mt_xs_comp_check(comp))) {
            /* the compartment died while we were in a call (and may have
             * already restarted at a higher generation): every handle and
             * stub into the old generation is stale from now on */
            Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
        }
    }
}

/*
=for apidoc mt_xs_next_seq

The next call sequence number of slot C<slot>.

=cut
*/

/*
=for apidoc mt_xs_inflight_max

The most fiber calls one worker may have in flight (PERL_MT_XS_INFLIGHT).

=cut
*/

U32
Perl_mt_xs_inflight_max(pTHX)
{
    PERL_UNUSED_CONTEXT;
    return S_pool.inflight;
}

/* Whether PERL_MT_XS_AFFINITY=smt is in effect (Linux pins; elsewhere the
 * knob is accepted and does nothing). */
bool
Perl_mt_xs_affinity(pTHX)
{
    PERL_UNUSED_CONTEXT;
    return S_pool.affinity ? TRUE : FALSE;
}

/*
=for apidoc mt_xs_arena_full

Whether slot C<slot>'s arena is more than half used (the host then holds
new calls back until the ones in flight are done, see S_arena_begin).

=cut
*/

bool
Perl_mt_xs_arena_full(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    return slot < S_pool.nslots
        && (size_t)atomic_load_explicit(&S_pool.slots[slot].bump,
                                        memory_order_acquire)
           > S_pool.arena_len / 2;
}

U32
Perl_mt_xs_next_seq(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    return S_pool.slot_seq[slot]++;
}

U32
Perl_mt_xs_comp_gen(pTHX_ U32 comp)
{
    PERL_UNUSED_CONTEXT;
    return comp < S_pool.ncomp ? S_pool.hc[comp].gen : 0;
}

/*
=for apidoc mt_xs_in_call_enter

Marks slot C<slot> as inside a remote call (nesting counts); the depth is
what callbacks nest against, and a depth of zero rewinds the arena.

=for apidoc mt_xs_in_call_leave

The matching exit.

=for apidoc mt_xs_in_call_depth

The current nesting depth of slot C<slot>.

=for apidoc mt_xs_slot_waiting

Inside a compartment: whether the worker of slot C<slot> is inside a call
to this compartment, i.e. will serve a C<CALLBACK> posted to it.

=cut
*/

void
Perl_mt_xs_in_call_enter(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    atomic_fetch_add_explicit(&S_pool.slots[slot].in_call, 1u, memory_order_acq_rel);
}

void
Perl_mt_xs_in_call_leave(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    atomic_fetch_sub_explicit(&S_pool.slots[slot].in_call, 1u, memory_order_acq_rel);
}

U32
Perl_mt_xs_in_call_depth(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    return atomic_load_explicit(&S_pool.slots[slot].in_call, memory_order_acquire);
}


/* ---- the child ------------------------------------------------------------- */

/*
 * A compartment is a child forked at the freeze.  It builds a fresh
 * interpreter (the parent's is left untouched in the copy), boots
 * DynaLoader and serves frames: LOAD requires a legacy module in-process
 * (perl_mt_active_workers() is zero in the child) and reports every XSUB
 * it defined; CALL and friends are served by mt_xs_bridge.c.  Everything
 * the compartment hands out to the host is pinned in the export table
 * until the host releases it (REL).
 */

typedef struct mt_xs_child {
    uint32_t          idx;
    uint32_t          gen;
    PerlInterpreter  *perl;
    HV               *exported;   /* SV address -> IV count (-1: for ever)  */
    HV               *xsubs;      /* full name -> RV to the XSUB            */
    HV               *mod_xsubs;  /* module -> AV of full names             */
    CV               *serve_cv;
    mt_xs_frame_t     cur;        /* the frame being served                 */
    int               cur_replied;
    mt_xs_frame_t    *deferred;   /* frames of other slots that arrived
                                     while a callback reply was awaited    */
    uint32_t          ndeferred, cap_deferred, deferred_head;
    uint32_t          waiting_slot;   /* slot whose CB_* we wait for        */
    uint32_t          waiting_depth;
    perl_krt_poll_policy_t poll;      /* the adaptive budget of the wait     */
    uint32_t          served;         /* frames since the last liveness check */
    mt_xs_frame_t    *cb;             /* callback replies that were not the one
                                          their nested wait wanted           */
    uint32_t          ncb, cap_cb;
} mt_xs_child_t;

static mt_xs_child_t S_child;

/* boot_DynaLoader is linked into the perl executable (not into libperl,
 * and not into miniperl at all), so it is looked up at run time; a host
 * without it gets compartments that cannot load */
static void
S_child_xs_init(pTHX)
{
    static const char file[] = __FILE__;
    void * const sym = dlsym(RTLD_DEFAULT, "boot_DynaLoader");
    if (sym)
        newXS("DynaLoader::boot_DynaLoader", DPTR2FPTR(XSUBADDR_t, sym), file);
}

/* a compartment hook snapshot: anything a legacy module changes here only
 * takes effect in the compartment (its own process), so a module whose
 * point is to hook the host's compiler is refused.  %MT::XS::hooks_ok on
 * the host waives the check for modules that hook incidentally: File::Glob
 * wraps PL_check[OP_GLOB] and PL_globhook at boot, which only serve glob
 * ops compiled in the compartment, where no code of the program's runs. */
typedef struct mt_xs_hooks {
    Perl_check_t          check[MAXO];
    Perl_keyword_plugin_t keyword_plugin;
    Perl_infix_plugin_t   infix_plugin;
    peep_t                peepp;
    peep_t                rpeepp;
    Perl_ophook_t         opfreehook;
    globhook_t            globhook;
    IV                    nlayers;
    IV                    ncustom;
} mt_xs_hooks_t;

static void
S_hooks_snapshot(pTHX_ mt_xs_hooks_t *h)
{
    Copy(PL_check, h->check, MAXO, Perl_check_t);
    h->keyword_plugin = PL_keyword_plugin;
    h->infix_plugin   = PL_infix_plugin;
    h->peepp          = PL_peepp;
    h->rpeepp         = PL_rpeepp;
    h->opfreehook     = PL_opfreehook;
    h->globhook       = PL_globhook;
    h->nlayers        = PL_known_layers ? PL_known_layers->cur : 0;
    h->ncustom        = PL_custom_ops ? (IV)HvUSEDKEYS(PL_custom_ops) : 0;
}

static bool
S_hooks_changed(pTHX_ const mt_xs_hooks_t *before)
{
    mt_xs_hooks_t now;
    S_hooks_snapshot(aTHX_ &now);
    if (memcmp(before, &now, sizeof now) == 0)
        return FALSE;
    if (PerlEnv_getenv("PERL_MT_XS_DEBUG")) {
        int i;
        PerlIO_printf(PerlIO_stderr(), "mt_xs[comp %u]: hooks changed:",
                      (unsigned)S_child.idx);
        for (i = 0; i < MAXO; i++)
            if (before->check[i] != now.check[i])
                PerlIO_printf(PerlIO_stderr(), " PL_check[%s]", PL_op_name[i]);
        if (before->keyword_plugin != now.keyword_plugin) PerlIO_printf(PerlIO_stderr(), " keyword_plugin");
        if (before->infix_plugin != now.infix_plugin) PerlIO_printf(PerlIO_stderr(), " infix_plugin");
        if (before->peepp != now.peepp) PerlIO_printf(PerlIO_stderr(), " peepp");
        if (before->rpeepp != now.rpeepp) PerlIO_printf(PerlIO_stderr(), " rpeepp");
        if (before->opfreehook != now.opfreehook) PerlIO_printf(PerlIO_stderr(), " opfreehook");
        if (before->globhook != now.globhook) PerlIO_printf(PerlIO_stderr(), " globhook");
        if (before->nlayers != now.nlayers) PerlIO_printf(PerlIO_stderr(), " layers(%ld->%ld)", (long)before->nlayers, (long)now.nlayers);
        if (before->ncustom != now.ncustom) PerlIO_printf(PerlIO_stderr(), " custom_ops");
        PerlIO_printf(PerlIO_stderr(), "\n");
    }
    return TRUE;
}

static void
S_hooks_restore(pTHX_ const mt_xs_hooks_t *h)
{
    Copy(h->check, PL_check, MAXO, Perl_check_t);
    PL_keyword_plugin = h->keyword_plugin;
    PL_infix_plugin   = h->infix_plugin;
    PL_peepp          = h->peepp;
    PL_rpeepp         = h->rpeepp;
    PL_opfreehook     = h->opfreehook;
    PL_globhook       = h->globhook;
}

/* ---- child: the export table ---------------------------------------------- */

/*
=for apidoc mt_xs_export_sv

Inside a compartment: pins C<sv> for the host (one more handle) and
returns the address the host's handle carries.  With C<permanent> the
pin is never released (XSUBs reported to the host).

=for apidoc mt_xs_exported_sv

Inside a compartment: the pinned SV behind the address a host handle or
stub carries, or NULL when the host sent something that was never
exported (or has been released).

=for apidoc mt_xs_unexport

Inside a compartment: one handle fewer on the exported SV (REL).

=cut
*/

U64
Perl_mt_xs_export_sv(pTHX_ SV *sv, bool permanent)
{
    SV **svp;
    PERL_ARGS_ASSERT_MT_XS_EXPORT_SV;
    svp = hv_fetch(S_child.exported, (const char *)&sv, sizeof sv, 1);
    if (SvOK(*svp)) {
        if (SvIV(*svp) > 0)
            sv_setiv(*svp, permanent ? -1 : SvIV(*svp) + 1);
    }
    else {
        sv_setiv(*svp, permanent ? -1 : 1);
        SvREFCNT_inc_simple_void_NN(sv);
    }
    return PTR2UV(sv);
}

SV *
Perl_mt_xs_exported_sv(pTHX_ U64 ptr)
{
    SV * const sv = INT2PTR(SV *, ptr);
    SV **svp;
    if (!sv)
        return NULL;
    svp = hv_fetch(S_child.exported, (const char *)&sv, sizeof sv, 0);
    return svp ? sv : NULL;
}

void
Perl_mt_xs_unexport(pTHX_ U64 ptr)
{
    SV * const sv = INT2PTR(SV *, ptr);
    SV **svp = sv ? hv_fetch(S_child.exported, (const char *)&sv, sizeof sv, 0)
                  : NULL;
    if (!svp)
        return;
    if (SvIV(*svp) < 0)
        return;                                   /* permanent */
    if (SvIV(*svp) > 1) {
        sv_setiv(*svp, SvIV(*svp) - 1);
        return;
    }
    (void)hv_delete(S_child.exported, (const char *)&sv, sizeof sv, G_DISCARD);
    SvREFCNT_dec(sv);
}

/*
=for apidoc mt_xs_in_compartment

True inside an XS compartment.

=for apidoc mt_xs_my_comp

Inside a compartment: its index in the pool.

=cut
*/

bool
Perl_mt_xs_in_compartment(void)
{
    return S_pool.child != 0;
}

U32
Perl_mt_xs_my_comp(void)
{
    return S_pool.child ? S_pool.child - 1 : 0;
}

U32
Perl_mt_xs_my_gen(void)
{
    return S_child.gen;
}

bool
Perl_mt_xs_slot_waiting(pTHX_ U32 slot)
{
    PERL_UNUSED_CONTEXT;
    if (slot >= S_pool.nslots)
        return FALSE;
    return atomic_load_explicit(&S_pool.slots[slot].in_use, memory_order_acquire)
        && atomic_load_explicit(&S_pool.slots[slot].in_call, memory_order_acquire) > 0
        && atomic_load_explicit(&S_pool.slots[slot].comp, memory_order_acquire)
           == S_child.idx;
}

/*
=for apidoc mt_xs_register_xsub

Inside a compartment: records the XSUB C<cv> under its full name C<name>
(pinned for ever) so that later loads of its module report it.

=cut
*/

void
Perl_mt_xs_register_xsub(pTHX_ SV *name, CV *cv)
{
    PERL_ARGS_ASSERT_MT_XS_REGISTER_XSUB;
    (void)hv_store_ent(S_child.xsubs, name, newRV_inc((SV *)cv), 0);
    (void)Perl_mt_xs_export_sv(aTHX_ (SV *)cv, TRUE);
}

/* ---- child: replies and the request stream ------------------------------- */

/*
=for apidoc mt_xs_reply

Inside a compartment: pushes frame C<f> on the reply ring of slot
C<slot> (the frame's slot), waiting while the ring is full as long as the
slot is still in use.

=cut
*/

void
Perl_mt_xs_reply(pTHX_ const mt_xs_frame_t *f)
{
    perl_krt_spsc_t *ring;
    PERL_ARGS_ASSERT_MT_XS_REPLY;
    if (f->slot >= S_pool.nslots)
        return;
    ring = S_reply_ring(f->slot);
    while (perl_krt_spsc_push(ring, f) != 0) {
        if (!atomic_load_explicit(&S_pool.slots[f->slot].in_use,
                                  memory_order_acquire))
            return;                               /* nobody will read it */
        /* full: the host has not taken the last replies yet */
        S_mt_xs_pause();
    }
    atomic_fetch_add_explicit(&S_pool.comps[S_child.idx].calls, 1u,
                              memory_order_relaxed);
    (void)perl_krt_vec_post(S_pool.vec, S_vec_slot(f->slot));
}

/* the simple string-vector codec used by LOAD and by error replies */
static void
S_put_u32(char **p, uint32_t v) { memcpy(*p, &v, 4); *p += 4; }
static void
S_put_u64(char **p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
static uint32_t
S_get_u32(const char **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static uint64_t
S_get_u64(const char **p) { uint64_t v; memcpy(&v, *p, 8); *p += 8; return v; }
static char *
S_mt_xs_shared_strdup(const char *s)
{
    const size_t len = strlen(s);
    char * const p = (char *)PerlMemShared_malloc(len + 1);
    memcpy(p, s, len + 1);
    return p;
}
static void
S_put_str(char **p, const char *s, uint32_t len)
{
    S_put_u32(p, len);
    memcpy(*p, s, len);
    *p += len;
}
static const char *
S_get_str(const char **p, uint32_t *len)
{
    const char *s;
    *len = S_get_u32(p);
    s = *p;
    *p += *len;
    return s;
}

/*
=for apidoc mt_xs_reply_die

Inside a compartment: answers the frame C<req> with C<DIE> carrying the
message C<msg> (the plain-string form used when the TLV codec is not
involved).

=cut
*/

void
Perl_mt_xs_reply_die_pv(pTHX_ const mt_xs_frame_t *req, const char *msg,
                        STRLEN len)
{
    mt_xs_frame_t r;
    char *p;
    U64 off;
    PERL_ARGS_ASSERT_MT_XS_REPLY_DIE_PV;
    Zero(&r, 1, mt_xs_frame_t);
    r.type = MT_XSF_DIE;
    r.slot = req->slot;
    r.seq = req->seq;
    r.depth = req->depth;
    r.flags = MT_XSF_FLAG_PLAIN_DIE;
    p = (char *)Perl_mt_xs_arena_alloc(aTHX_ req->slot, len + 4, &off);
    r.off = off;
    r.len = len + 4;
    S_put_str(&p, msg, (uint32_t)len);
    Perl_mt_xs_reply(aTHX_ &r);
}

static bool
S_frame_wants_reply(const mt_xs_frame_t *f)
{
    switch (f->type) {
    case MT_XSF_REL: case MT_XSF_CB_RET: case MT_XSF_CB_DIE:
    case MT_XSF_SLOT_FREE: case MT_XSF_CRASH:
        return FALSE;
    default:
        return TRUE;
    }
}

static void
S_child_defer(const mt_xs_frame_t *f)
{
    if (S_child.ndeferred == S_child.cap_deferred) {
        S_child.cap_deferred = S_child.cap_deferred ? S_child.cap_deferred * 2 : 64;
        Renew(S_child.deferred, S_child.cap_deferred, mt_xs_frame_t);
    }
    S_child.deferred[S_child.ndeferred++] = *f;
}

/* The next request: a deferred one first, else the ring (sleeping while
 * it is empty, and leaving when the host process is gone). */
static bool
S_child_next(mt_xs_frame_t *out)
{
    perl_krt_mpsc_t * const ring = S_req_ring(S_child.idx);
    if (S_child.deferred_head < S_child.ndeferred) {
        *out = S_child.deferred[S_child.deferred_head++];
        if (S_child.deferred_head == S_child.ndeferred)
            S_child.deferred_head = S_child.ndeferred = 0;
        return TRUE;
    }
    for (;;) {
        const uint32_t d = perl_krt_vec_doorbell(S_pool.vec, S_vec_comp(S_child.idx));
        /* a flood must not starve the liveness checks: after `batch`
         * frames, the host is looked at before the next one */
        if (S_child.served >= S_pool.batch) {
            S_child.served = 0;
            if (getppid() != S_pool.parent_pid)
                return FALSE;                     /* the host is gone */
        }
        if (perl_krt_mpsc_pop(ring, out, sizeof *out) == 0) {
            S_child.served++;
            return TRUE;
        }
        (void)perl_krt_vec_wait(S_pool.vec, S_vec_comp(S_child.idx), d,
                                &S_child.poll, MT_XS_WAIT_BACKSTOP_NS);
        if (getppid() != S_pool.parent_pid)
            return FALSE;                         /* the host is gone */
    }
}

static void S_child_serve(pTHX_ const mt_xs_frame_t *f);

/*
=for apidoc mt_xs_comp_wait_cb

Inside a compartment, while a host callback is outstanding for slot
C<slot> at nesting C<depth>: waits for its C<CB_RET> or C<CB_DIE> (copied
to C<*out>; returns false when the slot was released meanwhile).  Frames
from the same slot that arrive first are nested calls made by the
callback and are served at once; frames from other slots are deferred
until the current call is over, since the XSUB in flight must not be
re-entered by another worker.

=cut
*/

/* A CB_RET or CB_DIE that the wait on top of this one does not want: it is
 * the reply to an outer callback, which arrived while a nested call of the
 * same worker was served.  The outer wait takes it from here (with fibers,
 * the outer reply can be queued behind the nested call's frames). */
static void
S_cb_stash(const mt_xs_frame_t *f)
{
    if (S_child.ncb == S_child.cap_cb) {
        S_child.cap_cb = S_child.cap_cb ? S_child.cap_cb * 2 : 8;
        Renew(S_child.cb, S_child.cap_cb, mt_xs_frame_t);
    }
    S_child.cb[S_child.ncb++] = *f;
}

bool
Perl_mt_xs_comp_wait_cb(pTHX_ U32 slot, U32 depth, mt_xs_frame_t *out)
{
    perl_krt_mpsc_t * const ring = S_req_ring(S_child.idx);
    PERL_ARGS_ASSERT_MT_XS_COMP_WAIT_CB;
    for (;;) {
        mt_xs_frame_t f;
        uint32_t i;
        /* a reply that came while a nested call was served (below) */
        for (i = 0; i < S_child.ncb; i++) {
            if (S_child.cb[i].slot == slot && S_child.cb[i].depth == depth) {
                *out = S_child.cb[i];
                Move(&S_child.cb[i + 1], &S_child.cb[i], S_child.ncb - i - 1,
                     mt_xs_frame_t);
                S_child.ncb--;
                return TRUE;
            }
        }
        const uint32_t d = perl_krt_vec_doorbell(S_pool.vec, S_vec_comp(S_child.idx));
        if (perl_krt_mpsc_pop(ring, &f, sizeof f) != 0) {
            (void)perl_krt_vec_wait(S_pool.vec, S_vec_comp(S_child.idx), d,
                                    &S_child.poll, 200000000ull);
            if (getppid() != S_pool.parent_pid)
                _exit(0);
            if (!atomic_load_explicit(&S_pool.slots[slot].in_use,
                                      memory_order_acquire))
                return FALSE;                     /* the worker went away */
            continue;
        }
        if (f.type == MT_XSF_CB_RET || f.type == MT_XSF_CB_DIE) {
            if (f.slot == slot && f.depth == depth) {
                *out = f;
                return TRUE;
            }
            S_cb_stash(&f);                       /* an outer callback's reply */
            continue;
        }
        if (f.slot == slot || !S_frame_wants_reply(&f)) {
            /* a nested call from the worker we serve, or a release */
            S_child_serve(aTHX_ &f);
            continue;
        }
        S_child_defer(&f);
    }
}

/* ---- child: LOAD ----------------------------------------------------------- */

static SV *
S_cv_fullname(pTHX_ CV *cv)
{
    SV * const name = newSVpvs("");
    if (CvGV(cv))
        gv_fullname4(name, CvGV(cv), NULL, TRUE);
    return name;
}

/* require a module in the compartment and report the XSUBs the host must
 * have stubs for */
static void
S_child_load(pTHX_ const mt_xs_frame_t *f)
{
    const char *p = (const char *)Perl_mt_xs_arena_at(aTHX_ f->slot, f->off, f->len);
    uint32_t n = S_get_u32(&p), i, mlen, plen, first_inc;
    SSize_t k;
    const char *module, *path;
    AV *chain, *hooks_ok;
    HV *inc_before;
    SV *modsv, *errsv = NULL;
    mt_xs_hooks_t before;
    AV *log, *names;
    AV *inc = GvAVn(PL_incgv);
    SV **svp;

    if (n < 3)
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    module = S_get_str(&p, &mlen);
    path = S_get_str(&p, &plen);
    modsv = sv_2mortal(newSVpvn(module, mlen));
    chain = newAV();
    sv_2mortal((SV *)chain);
    {
        uint32_t clen, j;
        const char * const cstr = S_get_str(&p, &clen);
        const uint32_t nchain = (uint32_t)atoi(cstr);
        for (j = 0; j < nchain && 3 + j < n; j++) {
            uint32_t len;
            const char * const m = S_get_str(&p, &len);
            av_push(chain, newSVpvn(m, len));
        }
        first_inc = 3 + nchain;
    }
    hooks_ok = newAV();
    sv_2mortal((SV *)hooks_ok);
    if (first_inc < n) {
        uint32_t olen, j;
        const char * const ostr = S_get_str(&p, &olen);
        const uint32_t nok = (uint32_t)atoi(ostr);
        first_inc++;
        for (j = 0; j < nok && first_inc < n; j++, first_inc++) {
            uint32_t len;
            const char * const m = S_get_str(&p, &len);
            av_push(hooks_ok, newSVpvn(m, len));
        }
    }
    /* what %INC holds now: the modules this load adds are the suspects
     * when a hook changes */
    inc_before = newHV();
    sv_2mortal((SV *)inc_before);
    {
        HV * const incv = GvHVn(PL_incgv);
        HE *he;
        hv_iterinit(incv);
        while ((he = hv_iternext(incv)))
            (void)hv_store_ent(inc_before, hv_iterkeysv(he), &PL_sv_yes, 0);
    }

    /* the worker's @INC entries we do not have yet, in its order */
    {
        SV **add;
        uint32_t nadd = 0;
        Newx(add, n, SV *);
        SAVEFREEPV(add);
        for (i = first_inc; i < n; i++) {
            uint32_t len;
            const char * const dir = S_get_str(&p, &len);
            SSize_t j;
            bool have = FALSE;
            for (j = 0; j <= av_top_index(inc); j++) {
                SV ** const e = av_fetch(inc, j, 0);
                if (e && *e && SvPOK(*e) && SvCUR(*e) == len
                    && memEQ(SvPVX(*e), dir, len)) {
                    have = TRUE;
                    break;
                }
            }
            if (!have)
                add[nadd++] = newSVpvn(dir, len);
        }
        if (nadd) {
            av_unshift(inc, nadd);
            for (i = 0; i < nadd; i++)
                av_store(inc, i, add[i]);
        }
    }

    S_hooks_snapshot(aTHX_ &before);
    log = newAV();
    SAVEFREESV(log);
    SAVEVPTR(PL_mt_xs_newxs_log);   /* back to NULL however we leave */
    PL_mt_xs_newxs_log = log;

    if (!(f->flags & MT_XSF_FLAG_SO)) {
        /* the modules the host is requiring, outermost first, then the
         * module itself (a no-op when an outer one pulled it in) */
        SV * const code = sv_2mortal(newSVpvs(""));
        for (k = 0; k <= av_top_index(chain); k++)
            sv_catpvf(code, "require %" SVf "; ", SVfARG(*av_fetch(chain, k, 0)));
        sv_catpvf(code, "require %" SVf "; 1", SVfARG(modsv));
        /* $@ may hold the error of an earlier frame: only this eval's
         * failure counts (a false failure would boot the module twice
         * through the dlopen fallback below) */
        CLEAR_ERRSV();
        (void)eval_sv(code, G_SCALAR);
        if (SvTRUE(ERRSV))
            errsv = sv_mortalcopy(ERRSV);
    }
    if (errsv && PerlEnv_getenv("PERL_MT_XS_DEBUG"))
        PerlIO_printf(PerlIO_stderr(), "mt_xs[comp %u]: require %" SVf
                      " failed: %" SVf, (unsigned)S_child.idx, SVfARG(modsv),
                      SVfARG(errsv));
    if (errsv || (f->flags & MT_XSF_FLAG_SO)) {
        /* no .pm half (or the module name could not be told from the
         * path): load the shared object alone and run its boot */
        void *h = plen ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
        if (h) {
            SV * const bootname = sv_2mortal(newSVpvf("boot_%" SVf, SVfARG(modsv)));
            char *q;
            void *sym;
            for (q = SvPVX(bootname); *q; q++)
                if (!isALNUM(*q)) *q = '_';
            sym = dlsym(h, SvPVX(bootname));
            if (PerlEnv_getenv("PERL_MT_XS_DEBUG"))
                PerlIO_printf(PerlIO_stderr(), "mt_xs[comp %u]: dlopen fallback for %" SVf
                              " (%s): boot %s\n", (unsigned)S_child.idx, SVfARG(modsv),
                              path, sym ? "found" : "missing");
            if (sym) {
                SV * const bname = sv_2mortal(newSVpvf("%" SVf "::bootstrap", SVfARG(modsv)));
                CV * const boot = newXS_flags(SvPVX(bname), DPTR2FPTR(XSUBADDR_t, sym),
                                              path, NULL, XS_DYNAMIC_FILENAME);
                dSP;
                ENTER; SAVETMPS;
                PUSHMARK(SP);
                XPUSHs(modsv);
                PUTBACK;
                call_sv(MUTABLE_SV(boot), G_DISCARD | G_EVAL);
                FREETMPS; LEAVE;
                if (SvTRUE(ERRSV))
                    errsv = sv_mortalcopy(ERRSV);
                else
                    errsv = NULL;
            }
        }
        if (errsv)
            Perl_croak_sv(aTHX_ errsv);
        if (!h)
            Perl_croak(aTHX_ "perl5-mt: compartment cannot load %" SVf
                             ": %s (MT_NO_COMPARTMENT)",
                       SVfARG(modsv), plen ? dlerror() : "no shared object");
    }
    PL_mt_xs_newxs_log = NULL;

    if (!(f->flags & MT_XSF_FLAG_HOOKS_OK) && S_hooks_changed(aTHX_ &before)) {
        /* a module added to %INC by this load that the host vouches for
         * (File::Glob under IO::Compress, say) waives the refusal */
        bool waived = FALSE;
        HV * const incv = GvHVn(PL_incgv);
        HE *he;
        hv_iterinit(incv);
        while (!waived && (he = hv_iternext(incv))) {
            SV * const file = hv_iterkeysv(he);
            STRLEN flen;
            const char * const fpv = SvPV(file, flen);
            if (hv_exists_ent(inc_before, file, 0)
                || flen < 4 || !memEQ(fpv + flen - 3, ".pm", 3))
                continue;
            for (k = 0; k <= av_top_index(hooks_ok) && !waived; k++) {
                SV * const m = *av_fetch(hooks_ok, k, 0);
                /* Foo::Bar <-> Foo/Bar.pm */
                STRLEN j;
                const STRLEN mlen2 = SvCUR(m);
                const char * const mpv = SvPVX(m);
                STRLEN fi = 0;
                bool same = TRUE;
                for (j = 0; j < mlen2 && same; j++) {
                    if (mpv[j] == ':' && j + 1 < mlen2 && mpv[j + 1] == ':') {
                        same = fi < flen - 3 && fpv[fi] == '/';
                        j++;
                    }
                    else
                        same = fi < flen - 3 && fpv[fi] == mpv[j];
                    fi++;
                }
                if (same && fi == flen - 3)
                    waived = TRUE;
            }
        }
        if (!waived) {
            S_hooks_restore(aTHX_ &before);
            Perl_croak_mt(aTHX_ MT_COMPARTMENT_HOOK_UNSUPPORTED);
        }
    }

    /* register what was defined and attribute it to this module */
    svp = hv_fetch(S_child.mod_xsubs, module, mlen, 1);
    if (!SvROK(*svp))
        sv_setsv(*svp, sv_2mortal(newRV_noinc((SV *)newAV())));
    names = (AV *)SvRV(*svp);
    for (k = 0; k <= av_top_index(log); k++) {
        CV * const cv = (CV *)*av_fetch(log, k, 0);
        SV * const name = S_cv_fullname(aTHX_ cv);
        if (!SvCUR(name) || !CvISXSUB(cv)) {
            SvREFCNT_dec(name);
            continue;
        }
        Perl_mt_xs_register_xsub(aTHX_ name, cv);
        av_push(names, name);
    }

    /* the reply: the module's XSUBs by attribution, then by prefix */
    {
        HV * const seen = newHV();
        AV * const out = newAV();
        SV * const prefix = sv_2mortal(newSVpvf("%" SVf "::", SVfARG(modsv)));
        size_t bytes = 4;
        HE *he;
        char *w;
        U64 off;
        mt_xs_frame_t r;

        sv_2mortal((SV *)seen);
        sv_2mortal((SV *)out);
        for (k = 0; k <= av_top_index(names); k++) {
            SV * const name = *av_fetch(names, k, 0);
            if (hv_exists_ent(S_child.xsubs, name, 0) && !hv_exists_ent(seen, name, 0)) {
                (void)hv_store_ent(seen, name, &PL_sv_yes, 0);
                av_push(out, SvREFCNT_inc_simple_NN(name));
            }
        }
        hv_iterinit(S_child.xsubs);
        while ((he = hv_iternext(S_child.xsubs))) {
            SV * const name = hv_iterkeysv(he);
            if (SvCUR(name) > SvCUR(prefix)
                && memEQ(SvPVX(name), SvPVX(prefix), SvCUR(prefix))
                && !hv_exists_ent(seen, name, 0)) {
                (void)hv_store_ent(seen, name, &PL_sv_yes, 0);
                av_push(out, SvREFCNT_inc_simple_NN(name));
            }
        }
        for (k = 0; k <= av_top_index(out); k++) {
            SV * const name = *av_fetch(out, k, 0);
            SV ** const rv = hv_fetch(S_child.xsubs, SvPVX(name), SvCUR(name), 0);
            bytes += 4 + SvCUR(name) + 8 + 1 + 4;
            if (rv && SvPOK(SvRV(*rv)))
                bytes += SvCUR(SvRV(*rv));
        }

        /* constants ExtUtils::Constant::ProxySubs stored straight into the
         * module's stash (a scalar ref in a stash slot, no newXS: File::Glob,
         * POSIX, Fcntl, Socket ...), and constant subs (newCONSTSUB from a
         * boot, or `sub NAME () { 2 }` in a .pm, which Perl compiles to one):
         * their values go to the host, which makes constant subs of them
         * (a stub would be a round trip per use, and a later definition
         * of the same constant in the host's .pm would clash with it) */
        {
            HV * const stash = gv_stashsv(modsv, 0);
            AV * const consts = newAV();
            AV * const stubs = newAV();
            sv_2mortal((SV *)consts);
            sv_2mortal((SV *)stubs);
            for (k = 0; k <= av_top_index(out); k++) {
                SV * const name = *av_fetch(out, k, 0);
                SV ** const rv = hv_fetch(S_child.xsubs, SvPVX(name), SvCUR(name), 0);
                CV * const xcv = rv ? (CV *)SvRV(*rv) : NULL;
                SV *val;
                if (xcv && CvCONST(xcv) && (val = (SV *)CvXSUBANY(xcv).any_ptr)
                    && SvTYPE(val) < SVt_PVAV && !SvROK(val) && SvOK(val)) {
                    av_push(consts, SvREFCNT_inc_simple_NN(name));  /* qualified */
                    av_push(consts, newSVsv(val));
                }
                else
                    av_push(stubs, SvREFCNT_inc_simple_NN(name));
            }
            if (stash) {
                HE *he;
                hv_iterinit(stash);
                while ((he = hv_iternext(stash))) {
                    SV * const slot = HeVAL(he);
                    SV *val;
                    if (!slot || isGV_with_GP(slot) || !SvROK(slot))
                        continue;
                    val = SvRV(slot);
                    if (SvROK(val) || SvTYPE(val) > SVt_PVMG || !SvOK(val))
                        continue;
                    av_push(consts, newSVpvf("%" SVf "::%" SVf, SVfARG(modsv),
                                             SVfARG(sv_2mortal(newSVhek(HeKEY_hek(he))))));
                    av_push(consts, newSVsv(val));
                }
            }
            bytes += 4;
            for (k = 0; k <= av_top_index(consts); k += 2) {
                SV * const val = *av_fetch(consts, k + 1, 0);
                bytes += 4 + SvCUR(*av_fetch(consts, k, 0)) + 1 + 4
                       + (SvPOK(val) ? SvCUR(val) : 32);
            }

            Zero(&r, 1, mt_xs_frame_t);
            r.type = MT_XSF_RET;
            r.slot = f->slot;
            r.seq = f->seq;
            r.depth = f->depth;
            w = (char *)Perl_mt_xs_arena_alloc(aTHX_ f->slot, bytes, &off);
            r.off = off;
            S_put_u32(&w, (uint32_t)(av_top_index(stubs) + 1));
            for (k = 0; k <= av_top_index(stubs); k++) {
                SV * const name = *av_fetch(stubs, k, 0);
                SV ** const rv = hv_fetch(S_child.xsubs, SvPVX(name), SvCUR(name), 0);
                CV * const xcv = rv ? (CV *)SvRV(*rv) : NULL;
                S_put_str(&w, SvPVX(name), (uint32_t)SvCUR(name));
                S_put_u64(&w, xcv ? PTR2UV(xcv) : 0);
                if (xcv && SvPOK(xcv)) {
                    *w++ = 1;
                    S_put_str(&w, SvPVX(xcv), (uint32_t)SvCUR(xcv));
                }
                else {
                    *w++ = 0;
                    S_put_u32(&w, 0);
                }
            }
            S_put_u32(&w, (uint32_t)((av_top_index(consts) + 1) / 2));
            for (k = 0; k <= av_top_index(consts); k += 2) {
                SV * const name = *av_fetch(consts, k, 0);
                SV * const val = *av_fetch(consts, k + 1, 0);
                char tag;
                const char *vp;
                STRLEN vlen;
                char nbuf[32];
                S_put_str(&w, SvPVX(name), (uint32_t)SvCUR(name));
                if (SvPOK(val)) {
                    tag = SvUTF8(val) ? 'U' : 'p';
                    vp = SvPVX(val);
                    vlen = SvCUR(val);
                }
                else if (SvIOK(val)) {
                    tag = SvIsUV(val) ? 'u' : 'i';
                    (void)snprintf(nbuf, sizeof nbuf,
                                   SvIsUV(val) ? "%" UVuf : "%" IVdf,
                                   SvIsUV(val) ? (UV)SvUVX(val) : (UV)SvIVX(val));
                    vp = nbuf;
                    vlen = strlen(nbuf);
                }
                else {
                    tag = 'n';
                    (void)snprintf(nbuf, sizeof nbuf, "%.17g", (double)SvNVX(val));
                    vp = nbuf;
                    vlen = strlen(nbuf);
                }
                *w++ = tag;
                S_put_str(&w, vp, (uint32_t)vlen);
            }
            r.len = (uint64_t)(w - (char *)Perl_mt_xs_arena_at(aTHX_ f->slot, off, 0));
            if (PerlEnv_getenv("PERL_MT_XS_DEBUG"))
                PerlIO_printf(PerlIO_stderr(), "mt_xs[comp %u]: loaded %" SVf
                              ": %ld xsubs (%ld new), %ld stash constants\n",
                              (unsigned)S_child.idx, SVfARG(modsv),
                              (long)(av_top_index(stubs) + 1),
                              (long)(av_top_index(log) + 1),
                              (long)((av_top_index(consts) + 1) / 2));
            Perl_mt_xs_reply(aTHX_ &r);
            S_child.cur_replied = 1;
        }
    }
}

/* ---- child: dispatch ------------------------------------------------------- */

XS(XS_MT_XS__serve);
XS(XS_MT_XS__serve)
{
    dXSARGS;
    const mt_xs_frame_t f = S_child.cur;
    PERL_UNUSED_VAR(items);
    switch (f.type) {
    case MT_XSF_PING: {
        mt_xs_frame_t r;
        Zero(&r, 1, mt_xs_frame_t);
        r.type = MT_XSF_RET;
        r.slot = f.slot; r.seq = f.seq; r.depth = f.depth;
        Perl_mt_xs_reply(aTHX_ &r);
        S_child.cur_replied = 1;
        break;
    }
    case MT_XSF_CRASH:
        /* a debugging crash: _exit, not abort(), so that no core is written
         * (writing one takes seconds on a host that pipes cores to a
         * coredump service, and the death is what this frame exercises) */
        PerlIO_flush((PerlIO *)NULL);
        _exit(134);
    case MT_XSF_LOAD:
        S_child_load(aTHX_ &f);
        break;
    case MT_XSF_REL:
        Perl_mt_xs_unexport(aTHX_ f.a);
        break;
    case MT_XSF_CB_RET: case MT_XSF_CB_DIE: case MT_XSF_SLOT_FREE:
        break;                                    /* stale: nothing to do */
    default:
        /* CALL, METHOD, TIE_*, SETVAR, GETVAR: the bridge */
        Perl_mt_xs_comp_serve(aTHX_ &f);
        S_child.cur_replied = 1;
        break;
    }
    XSRETURN_EMPTY;
}

static void
S_child_serve(pTHX_ const mt_xs_frame_t *f)
{
    const mt_xs_frame_t saved = S_child.cur;
    const int saved_replied = S_child.cur_replied;
    dSP;

    S_child.cur = *f;
    S_child.cur_replied = 0;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    PUTBACK;
    call_sv(MUTABLE_SV(S_child.serve_cv), G_VOID | G_DISCARD | G_NOARGS | G_EVAL);
    SPAGAIN;
    if (SvTRUE(ERRSV) && !S_child.cur_replied && S_frame_wants_reply(f)) {
        STRLEN len;
        const char * const msg = SvPV(ERRSV, len);
        Perl_mt_xs_reply_die_pv(aTHX_ f, msg, len);
    }
    FREETMPS;
    LEAVE;
    S_child.cur = saved;
    S_child.cur_replied = saved_replied;
}

static int
S_mt_xs_comp_main(void *arg)
{
    const uint32_t idx = *(uint32_t *)arg;
    PerlInterpreter *parent = PERL_GET_THX;
    PerlInterpreter *my_perl;
    char **argv;
    int argc = 0;
    SSize_t i, ninc;
    AV *inc;
    dJMPENV;
    int jmp_rc;

    S_pool.child = (int)idx + 1;
    S_child.idx = idx;
    S_child.gen = atomic_load_explicit(&S_pool.comps[idx].gen, memory_order_acquire);
    /* the doorbells, inherited from the host's segment: attach checks the
     * table, then this compartment claims its request vector */
    S_pool.vec = perl_krt_vectab_attach(MT_XS_AT(S_pool.hdr->vec_off),
                                        S_pool.hdr->total - S_pool.hdr->vec_off);
    if (!S_pool.vec)
        return 1;
    (void)perl_krt_vec_claim(S_pool.vec, S_vec_comp(idx), (uint32_t)getpid());
    perl_krt_poll_policy_init(&S_child.poll, PERL_KRT_POLL_DEFAULT_MIN_NS,
                              PERL_KRT_POLL_DEFAULT_MAX_NS);
    if (S_pool.affinity && S_pool.comp_cpu[idx] >= 0)
        (void)perl_krt_cpu_pin(S_pool.comp_cpu[idx]);

    /* the arena and the image are the host's business */
    PL_mt_arena_mode = 0;
    PL_mt_ops_in_arena = 0;
    PL_mt_auto_freeze = 0;
    PL_mt_arena.base = NULL;

    /* perl -I<each of the host's @INC> -e 0 */
    {
        dTHXa(parent);
        PERL_UNUSED_VAR(my_perl);
        inc = GvAV(PL_incgv);
        ninc = inc ? av_top_index(inc) + 1 : 0;
        Newxz(argv, ninc + 4, char *);
        argv[argc++] = (char *)"perl";
        for (i = 0; i < ninc; i++) {
            SV ** const e = av_fetch(inc, i, 0);
            if (e && *e && SvPOK(*e)) {
                STRLEN len;
                const char * const dir = SvPV(*e, len);
                char * const a = (char *)safemalloc(len + 3);
                a[0] = '-'; a[1] = 'I';
                Copy(dir, a + 2, len, char);
                a[len + 2] = '\0';
                argv[argc++] = a;
            }
        }
        argv[argc++] = (char *)"-e";
        argv[argc++] = (char *)"0";
    }

    my_perl = perl_alloc();
    if (!my_perl)
        return 1;
    perl_construct(my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
    if (perl_parse(my_perl, S_child_xs_init, argc, argv, NULL) != 0)
        return 1;
    PERL_SET_PHASE(PERL_PHASE_RUN);
    S_child.perl = my_perl;
    S_child.exported = newHV();
    S_child.xsubs = newHV();
    S_child.mod_xsubs = newHV();
    S_child.serve_cv = newXS_flags("MT::XS::_serve", XS_MT_XS__serve, __FILE__, "", 0);
    S_child.waiting_slot = UINT32_MAX;

    atomic_store_explicit(&S_pool.comps[idx].pid, (uint32_t)getpid(),
                          memory_order_relaxed);
    atomic_store_explicit(&S_pool.comps[idx].alive, 1u, memory_order_release);

    JMPENV_PUSH(jmp_rc);
    if (jmp_rc == 0) {
        mt_xs_frame_t f;
        while (S_child_next(&f))
            S_child_serve(aTHX_ &f);
    }
    else if (jmp_rc == 2 && S_frame_wants_reply(&S_child.cur) && !S_child.cur_replied) {
        /* exit() inside an XSUB: tell the caller, then go */
        static const char msg[] =
            "perl5-mt: XS compartment called exit() (MT_COMPARTMENT_DEAD)";
        Perl_mt_xs_reply_die_pv(aTHX_ &S_child.cur, msg, sizeof msg - 1);
    }
    JMPENV_POP;
    atomic_store_explicit(&S_pool.comps[idx].alive, 0u, memory_order_release);
    PerlIO_flush((PerlIO *)NULL);
    return 0;
}

/* Pre-forked spare compartment: starts a clean Perl interpreter
 * from the pre-image state and sleeps on its spare doorbell vector until the
 * host promotes it to take over a crashed compartment. */
static int
S_mt_xs_spare_main(void *arg)
{
    const uint32_t sp = *(uint32_t *)arg;
    PerlInterpreter *parent = PERL_GET_THX;
    PerlInterpreter *my_perl;
    char **argv;
    int argc = 0;
    SSize_t i, ninc;
    AV *inc;
    uint32_t idx = UINT32_MAX;
    dJMPENV;
    int jmp_rc;

    S_pool.vec = perl_krt_vectab_attach(MT_XS_AT(S_pool.hdr->vec_off),
                                        S_pool.hdr->total - S_pool.hdr->vec_off);
    if (!S_pool.vec)
        return 1;
    (void)perl_krt_vec_claim(S_pool.vec, S_vec_spare(sp), (uint32_t)getpid());
    perl_krt_poll_policy_init(&S_child.poll, PERL_KRT_POLL_DEFAULT_MIN_NS,
                              PERL_KRT_POLL_DEFAULT_MAX_NS);

    PL_mt_arena_mode = 0;
    PL_mt_ops_in_arena = 0;
    PL_mt_auto_freeze = 0;
    PL_mt_arena.base = NULL;

    {
        dTHXa(parent);
        PERL_UNUSED_VAR(my_perl);
        inc = GvAV(PL_incgv);
        ninc = inc ? av_top_index(inc) + 1 : 0;
        Newxz(argv, ninc + 4, char *);
        argv[argc++] = (char *)"perl";
        for (i = 0; i < ninc; i++) {
            SV ** const e = av_fetch(inc, i, 0);
            if (e && *e && SvPOK(*e)) {
                STRLEN len;
                const char * const dir = SvPV(*e, len);
                char * const a = (char *)safemalloc(len + 3);
                a[0] = '-'; a[1] = 'I';
                Copy(dir, a + 2, len, char);
                a[len + 2] = '\0';
                argv[argc++] = a;
            }
        }
        argv[argc++] = (char *)"-e";
        argv[argc++] = (char *)"0";
    }

    my_perl = perl_alloc();
    if (!my_perl)
        return 1;
    perl_construct(my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
    if (perl_parse(my_perl, S_child_xs_init, argc, argv, NULL) != 0)
        return 1;
    PERL_SET_PHASE(PERL_PHASE_RUN);
    S_child.perl = my_perl;
    S_child.exported = newHV();
    S_child.xsubs = newHV();
    S_child.mod_xsubs = newHV();
    S_child.serve_cv = newXS_flags("MT::XS::_serve", XS_MT_XS__serve, __FILE__, "", 0);
    S_child.waiting_slot = UINT32_MAX;

    atomic_store_explicit(&S_pool.spares[sp].pid, (uint32_t)getpid(),
                          memory_order_relaxed);
    atomic_store_explicit(&S_pool.spares[sp].ready, 1u, memory_order_release);

    /* Wait until promoted to a compartment or told to shut down */
    for (;;) {
        const uint32_t d = perl_krt_vec_doorbell(S_pool.vec, S_vec_spare(sp));
        idx = atomic_load_explicit(&S_pool.spares[sp].assigned_comp,
                                   memory_order_acquire);
        if (idx < S_pool.ncomp)
            break;
        if (atomic_load_explicit(&S_pool.spares[sp].shutdown, memory_order_acquire)
            || getppid() != S_pool.parent_pid)
            return 0;
        (void)perl_krt_vec_wait(S_pool.vec, S_vec_spare(sp), d, NULL,
                                100000000ull);
    }

    S_pool.child = (int)idx + 1;
    S_child.idx = idx;
    S_child.gen = atomic_load_explicit(&S_pool.comps[idx].gen, memory_order_acquire);
    (void)perl_krt_vec_claim(S_pool.vec, S_vec_comp(idx), (uint32_t)getpid());
    if (S_pool.affinity && S_pool.comp_cpu[idx] >= 0)
        (void)perl_krt_cpu_pin(S_pool.comp_cpu[idx]);

    atomic_store_explicit(&S_pool.comps[idx].pid, (uint32_t)getpid(),
                          memory_order_relaxed);
    atomic_store_explicit(&S_pool.comps[idx].alive, 1u, memory_order_release);

    JMPENV_PUSH(jmp_rc);
    if (jmp_rc == 0) {
        mt_xs_frame_t f;
        while (S_child_next(&f))
            S_child_serve(aTHX_ &f);
    }
    else if (jmp_rc == 2 && S_frame_wants_reply(&S_child.cur) && !S_child.cur_replied) {
        static const char msg[] =
            "perl5-mt: XS compartment called exit() (MT_COMPARTMENT_DEAD)";
        Perl_mt_xs_reply_die_pv(aTHX_ &S_child.cur, msg, sizeof msg - 1);
    }
    JMPENV_POP;
    atomic_store_explicit(&S_pool.comps[idx].alive, 0u, memory_order_release);
    PerlIO_flush((PerlIO *)NULL);
    return 0;
}

/* ---- host: the legacy path of DynaLoader ---------------------------------- */

/*
 * While workers are active dl_load_file hands out a lazy library handle
 * instead of the dlopen handle: the decision between the two tiers needs
 * the boot symbol's name, which only dl_find_symbol has.  There a module
 * with the threadsafe marker stays in-process; one without is dlclosed
 * here, loaded into the worker's compartment (LOAD), every XSUB the
 * compartment reports gets a host stub, and the symbol handed back for
 * boot_X is a remote symbol whose stub (Pkg::bootstrap) is a no-op: the
 * compartment has already run the real boot.
 */

static mt_xs_lib_t **S_libs;
static uint32_t      S_nlibs, S_libs_cap;
static mt_xs_sym_t **S_syms;
static uint32_t      S_nsyms, S_syms_cap;

static mt_xs_lib_t *
S_lib_of(void *p)
{
    uint32_t i;
    mt_xs_lib_t *r = NULL;
    if (!S_pool.started)
        return NULL;
    MUTEX_LOCK(&S_pool.mutex);
    for (i = 0; i < S_nlibs; i++)
        if ((void *)S_libs[i] == p) {
            r = S_libs[i];
            break;
        }
    MUTEX_UNLOCK(&S_pool.mutex);
    return r && r->magic == MT_XS_LIB_MAGIC ? r : NULL;
}

static mt_xs_sym_t *
S_sym_of(void *p)
{
    uint32_t i;
    mt_xs_sym_t *r = NULL;
    if (!S_pool.started)
        return NULL;
    MUTEX_LOCK(&S_pool.mutex);
    for (i = 0; i < S_nsyms; i++)
        if ((void *)S_syms[i] == p) {
            r = S_syms[i];
            break;
        }
    MUTEX_UNLOCK(&S_pool.mutex);
    return r && r->magic == MT_XS_SYM_MAGIC ? r : NULL;
}

/*
=for apidoc mt_xs_dl_lib_new

C<dl_load_file> while workers are active: wraps the C<dlopen> handle and
the path in a lazy library handle (returned as the libref) so that
C<dl_find_symbol> can decide the tier.  Returns NULL when the dispatcher
is not active (no pool, or no worker alive), in which case DynaLoader
behaves as always.

=cut
*/

void *
Perl_mt_xs_dl_lib_new(pTHX_ void *handle, const char *path)
{
    mt_xs_lib_t *lib;
    PERL_ARGS_ASSERT_MT_XS_DL_LIB_NEW;
    if (!S_pool.started || S_pool.child || !S_pool.shm.base
        || perl_mt_active_workers(aTHX) == 0)
        return NULL;
    lib = (mt_xs_lib_t *)PerlMemShared_calloc(1, sizeof *lib);
    lib->magic = MT_XS_LIB_MAGIC;
    lib->handle = handle;
    lib->path = S_mt_xs_shared_strdup(path);
    MUTEX_LOCK(&S_pool.mutex);
    if (S_nlibs == S_libs_cap) {
        S_libs_cap = S_libs_cap ? S_libs_cap * 2 : 16;
        S_libs = (mt_xs_lib_t **)PerlMemShared_realloc(S_libs,
                                                      S_libs_cap * sizeof *S_libs);
    }
    S_libs[S_nlibs++] = lib;
    MUTEX_UNLOCK(&S_pool.mutex);
    return lib;
}

/* the module a boot symbol belongs to: boot_Foo__Bar -> Foo::Bar */
static SV *
S_module_of_boot(pTHX_ const char *bootname)
{
    SV * const sv = newSVpvs("");
    const char *p = bootname + 5;
    while (*p) {
        if (p[0] == '_' && p[1] == '_') {
            sv_catpvs(sv, "::");
            p += 2;
        }
        else
            sv_catpvn(sv, p++, 1);
    }
    return sv;
}

/*
=for apidoc mt_xs_install_stub

Installs (or refreshes) the host stub for the XSUB C<name> that lives at
C<remote_cv> in compartment C<comp> (generation C<gen>).  A sub of that
name already defined in-process is left alone; an older remote stub is
updated in place.  With C<reported> (an XSUB the compartment listed) a
C<DESTROY> or a C<bootstrap> gets no stub.  C<proto> (NULL: none) is the
XSUB's prototype over there.

=cut
*/

/* the host side of a reported DESTROY: a no-op, so that a Perl DESTROY in
 * the module's .pm half can call SUPER::DESTROY (XML::LibXML::Element) */
static void
S_mt_xs_stub_noop(pTHX_ CV *cv)
{
    dXSARGS;
    PERL_UNUSED_ARG(cv);
    PERL_UNUSED_VAR(items);
    XSRETURN_EMPTY;
}

static void
S_mt_xs_stub_croak(pTHX_ CV *cv)
{
    PERL_UNUSED_ARG(cv);
    /* the XSUB pointer of a remote stub is never meant to run: every
     * call goes through Perl_rpp_invoke_xs */
    Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
}

void
Perl_mt_xs_install_stub(pTHX_ const char *name, STRLEN len, U32 comp,
                        U32 gen, U64 remote_cv, bool reported,
                        const char *proto, STRLEN protolen)
{
    GV *gv;
    CV *cv;
    mt_xs_stub_t *st;
    PERL_ARGS_ASSERT_MT_XS_INSTALL_STUB;

    if (reported) {
        /* a class's DESTROY runs in the compartment when the object dies
         * there (REL); a remote stub would run it a second time, on a
         * handle or on a deep copy.  The host gets a no-op of that name
         * instead, unless it has a DESTROY of its own, so that an explicit
         * SUPER::DESTROY from the .pm half resolves */
        if (len > 9 && memEQ(name + len - 9, "::DESTROY", 9)) {
            gv = gv_fetchpvn_flags(name, len, GV_ADDMULTI, SVt_PVCV);
            if (!GvCV(gv))
                (void)newXS_len_flags(name, len, S_mt_xs_stub_noop,
                                      "mt_xs_bridge.c", NULL, NULL, 0);
            return;
        }
        /* a compartment's Pkg::bootstrap is its own boot wrapper:
         * XSLoader on the host jumps to an already-defined bootstrap,
         * which would boot the module a second time over there; the
         * host's loader makes its own no-op bootstrap for a remote module
         * (the reported == FALSE call below) */
        if (len > 11 && memEQ(name + len - 11, "::bootstrap", 11))
            return;
    }
    gv = gv_fetchpvn_flags(name, len, GV_ADDMULTI, SVt_PVCV);
    cv = GvCV(gv);
    if (cv && !CvMT_REMOTE(cv) && (!CvISXSUB(cv) || CvXSUB(cv) != NULL)
        && !(CvISXSUB(cv) && CvXSUB(cv) == S_mt_xs_stub_croak)) {
        /* the host has this sub in-process: it wins */
        if (CvROOT(cv) || CvISXSUB(cv))
            return;
    }
    if (cv && CvMT_REMOTE(cv)) {
        st = (mt_xs_stub_t *)SvPVX(CvXSUBANY(cv).any_sv);
    }
    else {
        SV *box;
        /* the compartment's prototype (an empty one for a constant), so
         * that a later `sub NAME ();` in the module's .pm matches */
        SV * const protosv = proto ? sv_2mortal(newSVpvn(proto, protolen)) : NULL;
        cv = newXS_len_flags(name, len, S_mt_xs_stub_croak, "mt_xs_bridge.c",
                             protosv ? SvPVX(protosv) : NULL, NULL, 0);
        box = newSV(sizeof(mt_xs_stub_t));
        SvPOK_only(box);
        SvCUR_set(box, sizeof(mt_xs_stub_t));
        CvXSUBANY(cv).any_sv = box;
        CvREFCOUNTED_ANYSV_on(cv);
        CvMT_REMOTE_on(cv);
        st = (mt_xs_stub_t *)SvPVX(box);
    }
    st->comp = (uint16_t)comp;
    st->gen = (uint16_t)gen;
    st->remote_cv = remote_cv;
}

/*
=for apidoc mt_xs_remote_load

Loads the legacy module C<module> (shared object C<path>) into this
interpreter's compartment and installs a stub for every XSUB it reports.
Dies with the compartment's error (a failed C<require>, or
C<MT_COMPARTMENT_HOOK_UNSUPPORTED>).  Returns the compartment.

=cut
*/

U32
Perl_mt_xs_remote_load(pTHX_ SV *module, const char *path, U32 flags)
{
    PERL_ARGS_ASSERT_MT_XS_REMOTE_LOAD;
    return Perl_mt_xs_remote_load_in(aTHX_ (U32)Perl_mt_xs_bind_comp(aTHX),
                                     module, path, flags);
}

/*
=for apidoc mt_xs_remote_load_in

As L</mt_xs_remote_load>, into compartment C<comp>.  With
C<MT_XSF_FLAG_PROXY> (C<MT::XS::remote>) the module is loaded but no stubs
are installed on the host: its objects are reached through proxies.

=cut
*/

/* The modules the host is in the middle of requiring, outermost first
 * (Foo::Bar for a Foo/Bar.pm frame): the compartment requires them too,
 * so that the .pm half that sets the package variables the XS reads
 * (YAML::XS for YAML::XS::LibYAML's shared object) exists there. */
static AV *
S_require_chain(pTHX)
{
    AV * const chain = newAV();
    const PERL_SI *si;
    sv_2mortal((SV *)chain);
    for (si = PL_curstackinfo; si; si = si->si_prev) {
        I32 ix;
        for (ix = si->si_cxix; ix >= 0; ix--) {
            const PERL_CONTEXT * const cx = &si->si_cxstack[ix];
            if (CxTYPE(cx) == CXt_EVAL && CxOLD_OP_TYPE(cx) == OP_REQUIRE
                && cx->blk_eval.old_namesv) {
                SV * const name = cx->blk_eval.old_namesv;
                STRLEN len;
                const char * const pv = SvPV(name, len);
                if (len > 3 && memEQ(pv + len - 3, ".pm", 3)
                    && pv[0] != '/' && pv[0] != '.') {
                    /* Foo/Bar.pm -> Foo::Bar */
                    SV * const mod = newSVpvs("");
                    STRLEN j;
                    for (j = 0; j < len - 3; j++) {
                        if (pv[j] == '/')
                            sv_catpvs(mod, "::");
                        else
                            sv_catpvn(mod, pv + j, 1);
                    }
                    av_unshift(chain, 1);
                    av_store(chain, 0, mod);
                }
            }
        }
    }
    return chain;
}

U32
Perl_mt_xs_remote_load_in(pTHX_ U32 comp, SV *module, const char *path, U32 flags)
{
    const U32 slot = Perl_mt_xs_slot(aTHX);
    AV * const inc = GvAV(PL_incgv);
    const SSize_t ninc = inc ? av_top_index(inc) + 1 : 0;
    AV * const chain = S_require_chain(aTHX);
    const SSize_t nchain = av_top_index(chain) + 1;
    HV *hooks_ok;
    uint32_t nok = 0;
    STRLEN mlen, plen = path ? strlen(path) : 0;
    const char * const mod = SvPV(module, mlen);
    size_t bytes = 4 + 4 + mlen + 4 + plen + 4 + 12;
    SSize_t i;
    char *w;
    U64 off;
    mt_xs_frame_t f, r;

    PERL_ARGS_ASSERT_MT_XS_REMOTE_LOAD_IN;

    if (!Perl_mt_xs_comp_alive(aTHX_ comp, Perl_mt_xs_comp_gen(aTHX_ comp)))
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    for (i = 0; i < nchain; i++)
        bytes += 4 + SvCUR(*av_fetch(chain, i, 0));
    hooks_ok = get_hv("MT::XS::hooks_ok", 0);
    bytes += 4 + 12;
    if (hooks_ok) {
        HE *he;
        hv_iterinit(hooks_ok);
        while ((he = hv_iternext(hooks_ok))) {
            bytes += 4 + HeKLEN(he);
            nok++;
        }
    }
    for (i = 0; i < ninc; i++) {
        SV ** const e = av_fetch(inc, i, 0);
        if (e && *e && SvPOK(*e))
            bytes += 4 + SvCUR(*e);
    }
    if (!atomic_load_explicit(&S_pool.slots[slot].in_call, memory_order_relaxed))
        Perl_mt_xs_arena_reset(aTHX_ slot);
    w = (char *)Perl_mt_xs_arena_alloc(aTHX_ slot, bytes, &off);
    Zero(&f, 1, mt_xs_frame_t);
    f.type = MT_XSF_LOAD;
    f.slot = slot;
    f.seq = Perl_mt_xs_next_seq(aTHX_ slot);
    f.flags = flags & MT_XSF_FLAG_SO;
    if (hooks_ok && hv_exists_ent(hooks_ok, module, 0))
        f.flags |= MT_XSF_FLAG_HOOKS_OK;
    f.off = off;
    {
        char * const start = w;
        uint32_t n = 3;
        char nbuf[12];
        S_put_u32(&w, 0);                         /* patched below */
        S_put_str(&w, mod, (uint32_t)mlen);
        S_put_str(&w, path ? path : "", (uint32_t)plen);
        (void)snprintf(nbuf, sizeof nbuf, "%u", (unsigned)nchain);
        S_put_str(&w, nbuf, (uint32_t)strlen(nbuf));
        for (i = 0; i < nchain; i++) {
            SV * const m = *av_fetch(chain, i, 0);
            S_put_str(&w, SvPVX(m), (uint32_t)SvCUR(m));
            n++;
        }
        /* the modules whose compiler hooks a compartment may ignore */
        (void)snprintf(nbuf, sizeof nbuf, "%u", (unsigned)nok);
        S_put_str(&w, nbuf, (uint32_t)strlen(nbuf));
        n++;
        if (hooks_ok) {
            HE *he;
            hv_iterinit(hooks_ok);
            while ((he = hv_iternext(hooks_ok))) {
                S_put_str(&w, HeKEY(he), (uint32_t)HeKLEN(he));
                n++;
            }
        }
        for (i = 0; i < ninc; i++) {
            SV ** const e = av_fetch(inc, i, 0);
            if (e && *e && SvPOK(*e)) {
                S_put_str(&w, SvPVX(*e), (uint32_t)SvCUR(*e));
                n++;
            }
        }
        memcpy(start, &n, 4);
        f.len = (uint64_t)(w - start);
    }
    {
        void *saved_payload = NULL;
        size_t saved_len = (size_t)f.len;
        if (saved_len > 0) {
            saved_payload = malloc(saved_len);
            if (saved_payload)
                memcpy(saved_payload,
                       Perl_mt_xs_arena_at(aTHX_ slot, f.off, f.len),
                       saved_len);
        }
        atomic_fetch_add_explicit(&S_pool.slots[slot].in_call, 1u, memory_order_relaxed);
        Perl_mt_xs_post(aTHX_ comp, &f);
        {
            dJMPENV;
            int rc;
            JMPENV_PUSH(rc);
            if (rc == 0)
                Perl_mt_xs_wait(aTHX_ comp, slot, f.seq, &r);
            JMPENV_POP;
            atomic_fetch_sub_explicit(&S_pool.slots[slot].in_call, 1u, memory_order_relaxed);
            if (rc != 0) {
                free(saved_payload);
                JMPENV_JUMP(rc);
            }
        }
        if (r.type == MT_XSF_DIE) {
            const char *p = (const char *)Perl_mt_xs_arena_at(aTHX_ slot, r.off, r.len);
            uint32_t len;
            const char * const msg = S_get_str(&p, &len);
            free(saved_payload);
            Perl_croak_sv(aTHX_ sv_2mortal(newSVpvn(msg, len)));
        }
        if (saved_payload) {
            S_mt_xs_record_loaded_mod(comp, mod, mlen, f.flags,
                                      saved_payload, saved_len);
            free(saved_payload);
        }
    }
    if (PerlEnv_getenv("PERL_MT_XS_DEBUG")) {
        const char *p = (const char *)Perl_mt_xs_arena_at(aTHX_ slot, r.off, r.len);
        PerlIO_printf(PerlIO_stderr(), "mt_xs[worker %u]: LOAD %" SVf
                      " into compartment %u: %u xsubs reported%s\n",
                      (unsigned)PL_mt_worker_id, SVfARG(module), (unsigned)comp,
                      (unsigned)S_get_u32(&p),
                      (flags & MT_XSF_FLAG_PROXY) ? " (proxy: no stubs)" : "");
    }
    if (!(flags & MT_XSF_FLAG_PROXY)) {
        const char *p = (const char *)Perl_mt_xs_arena_at(aTHX_ slot, r.off, r.len);
        uint32_t n = S_get_u32(&p), k;
        const U32 gen = Perl_mt_xs_comp_gen(aTHX_ comp);
        for (k = 0; k < n; k++) {
            uint32_t len, plen2;
            const char * const name = S_get_str(&p, &len);
            const uint64_t ptr = S_get_u64(&p);
            const char has_proto = *p++;
            const char * const proto = S_get_str(&p, &plen2);
            if (ptr)
                Perl_mt_xs_install_stub(aTHX_ name, len, comp, gen, ptr, TRUE,
                                        has_proto ? proto : NULL, plen2);
        }
        /* the module's stash constants (ProxySubs): plain constant subs
         * here, the value being all there is to them */
        if (p + 4 <= (const char *)Perl_mt_xs_arena_at(aTHX_ slot, r.off, r.len) + r.len) {
            n = S_get_u32(&p);
            for (k = 0; k < n; k++) {
                uint32_t nlen, vlen;
                const char * const name = S_get_str(&p, &nlen);   /* qualified */
                const char tag = *p++;
                const char * const vp = S_get_str(&p, &vlen);
                SV *val;
                GV * const gv = gv_fetchpvn_flags(name, nlen, 0, SVt_PVCV);
                if (gv && isGV_with_GP(gv) && GvCV(gv))
                    continue;                     /* the host has its own */
                switch (tag) {
                case 'i': val = newSViv(Atol(vp)); break;
                case 'u': val = newSVuv((UV)Strtoul(vp, NULL, 10)); break;
                case 'n': val = newSVnv(Atof(vp)); break;
                default:
                    val = newSVpvn(vp, vlen);
                    if (tag == 'U')
                        SvUTF8_on(val);
                    break;
                }
                (void)newCONSTSUB_flags(NULL, name, nlen, 0, val);
            }
        }
    }
    return comp;
}

/*
=for apidoc mt_xs_dl_find_symbol

C<dl_find_symbol> on a lazy library handle: returns false when C<libref>
is not one (DynaLoader then does its usual C<dlsym>).  Otherwise the
symbol is looked up; for C<boot_X> the threadsafe marker decides between
the in-process boot (C<PL_mt_boot_safe_sym> is set for the boot wrapper)
and the compartment (the module is loaded there and a remote symbol is
returned).  C<*out> receives the IV to hand back, or 0 with the error
message in C<*err>.

=cut
*/

bool
Perl_mt_xs_dl_find_symbol(pTHX_ void *libref, const char *symbolname,
                          IV *out, const char **err)
{
    mt_xs_lib_t * const lib = S_lib_of(libref);
    void *sym;
    PERL_ARGS_ASSERT_MT_XS_DL_FIND_SYMBOL;

    *out = 0;
    *err = NULL;
    if (!lib)
        return FALSE;
    if (!lib->handle) {
        /* already loaded into a compartment: only its boot is known */
        *err = "library is served by an XS compartment";
        return TRUE;
    }
    sym = dlsym(lib->handle, symbolname);
    if (!sym) {
        *err = dlerror();
        return TRUE;
    }
    if (!strBEGINs(symbolname, "boot_")) {
        *out = PTR2IV(sym);
        return TRUE;
    }
    {
        const char * const marker = Perl_form(aTHX_ "mt_threadsafe_%s", symbolname);
        SV * const module = sv_2mortal(S_module_of_boot(aTHX_ symbolname));
        HV * const ok = get_hv("MT::XS::inprocess_ok", 0);
        if (dlsym(lib->handle, marker)) {
            PL_mt_boot_safe_sym = sym;
            *out = PTR2IV(sym);
            return TRUE;
        }
        if (ok && hv_exists_ent(ok, module, 0)) {
            *out = PTR2IV(sym);               /* vouched for: in-process */
            return TRUE;
        }
        /* the legacy tier: the compartment loads it, the host keeps
         * nothing of the shared object */
        dlclose(lib->handle);
        lib->handle = NULL;
        lib->comp = Perl_mt_xs_remote_load(aTHX_ module, lib->path, 0);
        lib->gen = Perl_mt_xs_comp_gen(aTHX_ lib->comp);
        {
            mt_xs_sym_t * const rs = (mt_xs_sym_t *)PerlMemShared_calloc(1, sizeof *rs);
            rs->magic = MT_XS_SYM_MAGIC;
            rs->stub.comp = (uint16_t)lib->comp;
            rs->stub.gen = (uint16_t)lib->gen;
            rs->stub.remote_cv = 0;           /* the bootstrap stub is a no-op */
            rs->module = S_mt_xs_shared_strdup(SvPVX(module));
            MUTEX_LOCK(&S_pool.mutex);
            if (S_nsyms == S_syms_cap) {
                S_syms_cap = S_syms_cap ? S_syms_cap * 2 : 16;
                S_syms = (mt_xs_sym_t **)PerlMemShared_realloc(S_syms,
                                                              S_syms_cap * sizeof *S_syms);
            }
            S_syms[S_nsyms++] = rs;
            MUTEX_UNLOCK(&S_pool.mutex);
            *out = PTR2IV(rs);
        }
        return TRUE;
    }
}

/*
=for apidoc mt_xs_dl_unload

C<dl_unload_file> on a lazy library handle: returns false when C<libref>
is not one.

=cut
*/

bool
Perl_mt_xs_dl_unload(pTHX_ void *libref)
{
    mt_xs_lib_t * const lib = S_lib_of(libref);
    PERL_UNUSED_CONTEXT;
    if (!lib)
        return FALSE;
    if (lib->handle)
        dlclose(lib->handle);
    lib->handle = NULL;
    return TRUE;
}

/* dl_install_xsub of a remote boot symbol: the no-op bootstrap stub */
static CV *
S_mt_xs_install_remote_boot(pTHX_ const char *name, mt_xs_sym_t *rs)
{
    const STRLEN len = strlen(name);
    GV *gv;
    Perl_mt_xs_install_stub(aTHX_ name, len, rs->stub.comp, rs->stub.gen, 0, FALSE,
                            NULL, 0);
    gv = gv_fetchpvn_flags(name, len, 0, SVt_PVCV);
    return gv ? GvCV(gv) : NULL;
}

/* ---- host: remote handles ------------------------------------------------ */

/*
=for apidoc mt_xs_handle_new

A new remote handle: a reference to a scalar carrying C<PERL_MAGIC_mt_cap>
of kind C<MT_CAP_KIND_REMOTE> that names the SV at C<ptr> inside
compartment C<comp> (generation C<gen>).  C<rclass> is the class the
compartment's value had (kept in the record); the handle itself is
blessed into C<blessinto> when given (the real class, C<MT::XS::Proxy::Obj>
for the objects of an C<MT::XS::remote> module, or a tie class when the
handle is the tie object of a hash or array shape).  Its DESTROY releases
the compartment's pin (C<REL>).

=for apidoc mt_xs_handle_ref

The remote handle record behind C<sv> (or what it refers to), or NULL when
C<sv> is not a remote handle.

=for apidoc mt_xs_handle_free

Called by the handle's magic as it is freed: posts C<REL> to the
compartment when it is still the one the handle was made for.

=cut
*/

SV *
Perl_mt_xs_handle_new(pTHX_ U32 comp, U32 gen, U64 ptr, U8 reftype,
                      bool proxy, const char *rclass, STRLEN rclasslen,
                      const char *blessinto, STRLEN blesslen)
{
    SV * const inner = newSV_type(SVt_PVMG);
    perl_mt_cap_ref_t ref;
    MAGIC *mg;
    SV *rv;

    Zero(&ref, 1, perl_mt_cap_ref_t);
    ref.kind = MT_CAP_KIND_REMOTE;
    ref.comp = (uint16_t)comp;
    ref.cgen = (uint16_t)gen;
    ref.ptr = ptr;
    ref.reftype = reftype;
    ref.proxy = proxy ? 1 : 0;
    ref.rclass = rclass && rclasslen ? savepvn(rclass, rclasslen) : NULL;
    mg = sv_magicext(inner, NULL, PERL_MAGIC_mt_cap, &PL_vtbl_mt_cap,
                     (const char *)&ref, (I32)sizeof ref);
    mg->mg_flags |= MGf_DUP;
    rv = newRV_noinc(inner);
    if (blessinto && blesslen)
        sv_bless(rv, gv_stashpvn(blessinto, blesslen, GV_ADD));
    return rv;
}

perl_mt_cap_ref_t *
Perl_mt_xs_handle_ref(pTHX_ SV *sv)
{
    MAGIC *mg;
    PERL_ARGS_ASSERT_MT_XS_HANDLE_REF;
    if (SvROK(sv))
        sv = SvRV(sv);
    if (SvTYPE(sv) < SVt_PVMG || !SvRMAGICAL(sv))
        return NULL;
    mg = mg_findext(sv, PERL_MAGIC_mt_cap, &PL_vtbl_mt_cap);
    if (!mg)
        return NULL;
    {
        perl_mt_cap_ref_t * const ref = (perl_mt_cap_ref_t *)mg->mg_ptr;
        return ref->kind == MT_CAP_KIND_REMOTE ? ref : NULL;
    }
}

void
Perl_mt_xs_handle_free(pTHX_ perl_mt_cap_ref_t *ref)
{
    PERL_ARGS_ASSERT_MT_XS_HANDLE_FREE;
    if (ref->ptr && S_pool.shm.base && !S_pool.child
        && ref->comp < S_pool.ncomp
        && S_pool.hc[ref->comp].gen == ref->cgen
        && !S_pool.hc[ref->comp].dead) {
        mt_xs_frame_t f;
        perl_krt_mpsc_t * const ring = S_req_ring(ref->comp);
        int tries = 1000;
        Zero(&f, 1, mt_xs_frame_t);
        f.type = MT_XSF_REL;
        f.slot = PL_mt_xs_slot >= 0 ? (uint32_t)PL_mt_xs_slot : UINT32_MAX;
        f.a = ref->ptr;
        /* never block in a destructor: a full ring means the compartment
         * is far behind; the pin is leaked rather than the worker stalled */
        while (perl_krt_mpsc_push(ring, &f, sizeof f) != 0 && --tries)
            perl_krt_cpu_relax();
    }
    ref->ptr = 0;
    if (ref->rclass) {
        Safefree(ref->rclass);
        ref->rclass = NULL;
    }
}

/* ---- MT::XS ------------------------------------------------------------ */


static CV *
S_mt_xs_cv_arg(pTHX_ SV *sv)
{
    if (SvROK(sv) && SvTYPE(SvRV(sv)) == SVt_PVCV)
        return (CV *)SvRV(sv);
    if (SvTYPE(sv) == SVt_PVCV)
        return (CV *)sv;
    {
        GV * const gv = gv_fetchsv(sv, 0, SVt_PVCV);
        return gv && isGV(gv) ? GvCV(gv) : NULL;
    }
}

XS(XS_MT_XS_is_threadsafe);
XS(XS_MT_XS_is_threadsafe)
{
    dXSARGS;
    CV *target;
    if (items != 1)
        croak_xs_usage(cv, "code");
    target = S_mt_xs_cv_arg(aTHX_ ST(0));
    ST(0) = target && CvISXSUB(target) && CvMT_THREADSAFE(target)
            ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

XS(XS_MT_XS_is_remote);
XS(XS_MT_XS_is_remote)
{
    dXSARGS;
    CV *target;
    if (items != 1)
        croak_xs_usage(cv, "code");
    target = S_mt_xs_cv_arg(aTHX_ ST(0));
    ST(0) = target && CvISXSUB(target) && CvMT_REMOTE(target)
            ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

XS(XS_MT_XS_active_workers);
XS(XS_MT_XS_active_workers)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSVuv(perl_mt_active_workers(aTHX)));
    XSRETURN(1);
}


/* a round trip to compartment `comp` carrying no payload */
static void
S_mt_xs_ping(pTHX_ U32 comp)
{
    mt_xs_frame_t f, r;
    const U32 slot = Perl_mt_xs_slot(aTHX);
    if (!Perl_mt_xs_comp_alive(aTHX_ comp, Perl_mt_xs_comp_gen(aTHX_ comp)))
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    Zero(&f, 1, mt_xs_frame_t);
    f.type = MT_XSF_PING;
    f.slot = slot;
    f.seq = Perl_mt_xs_next_seq(aTHX_ slot);
    Perl_mt_xs_post(aTHX_ comp, &f);
    Perl_mt_xs_wait(aTHX_ comp, slot, f.seq, &r);
    if (r.type == MT_XSF_DIE)
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
}

XS(XS_MT_XS_compartments);
XS(XS_MT_XS_compartments)
{
    dXSARGS;
    uint32_t i;
    PERL_UNUSED_VAR(items);
    SP -= items;
    if (S_pool.shm.base) {
        for (i = 0; i < S_pool.ncomp; i++) {
            HV * const hv = newHV();
            const bool alive = S_mt_xs_comp_check(i);
            (void)hv_stores(hv, "id", newSVuv(i));
            (void)hv_stores(hv, "pid", newSViv(S_pool.hc[i].proc.pid));
            (void)hv_stores(hv, "gen", newSVuv(S_pool.hc[i].gen));
            (void)hv_stores(hv, "alive", newSViv(alive ? 1 : 0));
            (void)hv_stores(hv, "ready",
                            newSViv(atomic_load_explicit(&S_pool.comps[i].alive,
                                                         memory_order_acquire)));
            (void)hv_stores(hv, "calls",
                            newSVuv(atomic_load_explicit(&S_pool.comps[i].calls,
                                                         memory_order_relaxed)));
            (void)hv_stores(hv, "restarts", newSVuv(S_pool.hc[i].restarts));
            (void)hv_stores(hv, "last_restart_ns",
                            newSVuv((UV)S_pool.hc[i].last_restart_ns));
            (void)hv_stores(hv, "failed_inflight",
                            newSVuv((UV)S_pool.hc[i].failed_inflight));
            (void)hv_stores(hv, "loaded_modules",
                            newSVuv((UV)S_pool.hc[i].nloaded));
            XPUSHs(sv_2mortal(newRV_noinc((SV *)hv)));
        }
    }
    PUTBACK;
}

XS(XS_MT_XS_ping);
XS(XS_MT_XS_ping)
{
    dXSARGS;
    U32 comp;
    if (items > 1)
        croak_xs_usage(cv, "[compartment]");
    comp = items ? (U32)SvUV(ST(0)) : (U32)Perl_mt_xs_bind_comp(aTHX);
    if (comp >= Perl_mt_xs_comp_count(aTHX))
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    S_mt_xs_ping(aTHX_ comp);
    XSRETURN_YES;
}

XS(XS_MT_XS__crash);
XS(XS_MT_XS__crash)
{
    dXSARGS;
    mt_xs_frame_t f;
    U32 comp;
    if (items != 1)
        croak_xs_usage(cv, "compartment");
    comp = (U32)SvUV(ST(0));
    if (comp >= Perl_mt_xs_comp_count(aTHX))
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    Zero(&f, 1, mt_xs_frame_t);
    f.type = MT_XSF_CRASH;
    f.slot = UINT32_MAX;
    Perl_mt_xs_post(aTHX_ comp, &f);
    XSRETURN_YES;
}

XS(XS_MT_XS_stats);
XS(XS_MT_XS_stats)
{
    dXSARGS;
    HV * const hv = newHV();
    uint32_t i, used = 0, spares_avail = 0, total_restarts = 0;
    PERL_UNUSED_VAR(items);
    if (S_pool.shm.base) {
        for (i = 0; i < S_pool.worker_slots; i++)
            if (atomic_load_explicit(&S_pool.slots[i].in_use, memory_order_relaxed))
                used++;
        for (i = 0; i < S_pool.nspares; i++)
            if (!S_pool.hsp[i].used && !S_pool.hsp[i].dead)
                spares_avail++;
        for (i = 0; i < S_pool.ncomp; i++)
            total_restarts += S_pool.hc[i].restarts;
    }
    (void)hv_stores(hv, "compartments", newSVuv(S_pool.shm.base ? S_pool.ncomp : 0));
    (void)hv_stores(hv, "slots", newSVuv(S_pool.shm.base ? S_pool.worker_slots : 0));
    (void)hv_stores(hv, "slots_used", newSVuv(used));
    (void)hv_stores(hv, "spares_total", newSVuv(S_pool.shm.base ? S_pool.nspares : 0));
    (void)hv_stores(hv, "spares_available", newSVuv(spares_avail));
    (void)hv_stores(hv, "restarts", newSVuv(total_restarts));
    (void)hv_stores(hv, "arena_bytes", newSVuv(S_pool.arena_len));
    (void)hv_stores(hv, "slot", newSViv(PL_mt_xs_slot));
    (void)hv_stores(hv, "compartment", newSViv(PL_mt_xs_comp));
    (void)hv_stores(hv, "in_compartment", newSViv(S_pool.child ? 1 : 0));
    ST(0) = sv_2mortal(newRV_noinc((SV *)hv));
    XSRETURN(1);
}

void
Perl_mt_boot_xs(pTHX)
{
    newXS_flags("MT::XS::compartments", XS_MT_XS_compartments, __FILE__, "", 0);
    newXS_flags("MT::XS::ping",         XS_MT_XS_ping,         __FILE__, ";$", 0);
    newXS_flags("MT::XS::_crash",       XS_MT_XS__crash,       __FILE__, "$", 0);
    newXS_flags("MT::XS::stats",        XS_MT_XS_stats,        __FILE__, "", 0);
    Perl_mt_xs_boot_bridge(aTHX);
    newXS_flags("MT::XS::is_threadsafe", XS_MT_XS_is_threadsafe, __FILE__, "$", 0);
    newXS_flags("MT::XS::is_remote",     XS_MT_XS_is_remote,     __FILE__, "$", 0);
    newXS_flags("MT::XS::active_workers", XS_MT_XS_active_workers, __FILE__, "", 0);
    /* %MT::XS::inprocess_ok: modules the user vouches for (loaded
     * in-process even without the marker; MY_CXT check waived);
     * %MT::XS::hooks_ok: modules whose compiler hooks a compartment may
     * ignore (they only serve code compiled there) */
    (void)get_hv("MT::XS::inprocess_ok", GV_ADD | GV_ADDMULTI);
    {
        HV * const ok = get_hv("MT::XS::hooks_ok", GV_ADD | GV_ADDMULTI);
        (void)hv_stores(ok, "File::Glob", newSViv(1));
    }
}

/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

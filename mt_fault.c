/*    mt_fault.c
 *
 *    perl5-mt: fail-fast policy, fault event model, and
 *    supervisor liveness heartbeats over the krt vector table (krt_vec.h).
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_FAULT_C
#include "perl.h"
#include "mt_fault.h"
#include "krt/krt_vec.h"
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- fault event ring & classification table ------------------ */

#define MT_FAULT_RING_CAP 64u

static pthread_mutex_t S_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        S_fault_seq = 0;
static perl_mt_fault_t S_fault_last;
static perl_mt_fault_t S_fault_ring[MT_FAULT_RING_CAP];

/* Complete classification of panic / croak_mt / abort sites in mt_*.c:
 *   MT_FAULT_RECOVERABLE: offending worker or compartment stops at once;
 *                         host process continues and observes the fault.
 *   MT_FAULT_FATAL:       unrecoverable process-wide invariant violation;
 *                         process terminates immediately (fail-fast). */
static const perl_mt_fault_site_t S_fault_sites[] = {
    /* mt_cap.c */
    { "mt_cap.c", "S_mt_cap_table", "MT_WORKER_FAILED", MT_FAULT_FATAL,
      "worker started without process capability table" },
    { "mt_cap.c", "Perl_mt_cap_mint", "MT_WORKER_FAILED", MT_FAULT_RECOVERABLE,
      "capability table exhausted; mint fails in calling unit" },
    { "mt_cap.c", "S_mt_cap_live", "MT_STALE_CAP", MT_FAULT_RECOVERABLE,
      "stale or invalid capability handle used by unit" },
    { "mt_cap.c", "Perl_mt_cap_drop", "MT_STALE_CAP", MT_FAULT_RECOVERABLE,
      "drop called on non-capability scalar" },

    /* mt_chan.c */
    { "mt_chan.c", "S_obj_add", "ENOMEM", MT_FAULT_FATAL,
      "process-wide object table allocation failed" },
    { "mt_chan.c", "S_obj_get", "MT_NO_SUCH_OBJECT", MT_FAULT_RECOVERABLE,
      "invalid channel, cell, or shared hash id" },
    { "mt_chan.c", "XS_MT_Channel__new", "MT_NO_SUCH_OBJECT", MT_FAULT_RECOVERABLE,
      "invalid channel capacity requested" },
    { "mt_chan.c", "XS_MT_Channel__send", "MT_CHAN_CLOSED", MT_FAULT_RECOVERABLE,
      "send on a closed channel" },

    /* mt_fiber.c */
    { "mt_fiber.c", "S_fiber_alloc_stack", "MT_CTX_STACK", MT_FAULT_RECOVERABLE,
      "fiber guard stack mmap/mprotect failed" },
    { "mt_fiber.c", "S_fiber_trampoline", "abort", MT_FAULT_FATAL,
      "unreachable return from finished fiber context" },
    { "mt_fiber.c", "Perl_mt_fiber_resume", "MT_NOT_A_FIBER/MT_FIBER_BUSY", MT_FAULT_RECOVERABLE,
      "invalid fiber state transition in unit" },
    { "mt_fiber.c", "Perl_mt_fiber_yield", "MT_NOT_IN_FIBER/MT_YIELD_IN_COMPILE", MT_FAULT_RECOVERABLE,
      "illegal fiber yield outside fiber or during compile" },
    { "mt_fiber.c", "Perl_mt_sched_run", "MT_SCHED_NESTED/MT_FIBER_DEADLOCK", MT_FAULT_RECOVERABLE,
      "nested scheduler loop or deadlocked fiber set in unit" },

    /* mt_freeze.c */
    { "mt_freeze.c", "perl_freeze_boot_image", "MT_ALREADY_FROZEN", MT_FAULT_RECOVERABLE,
      "duplicate boot image freeze call" },
    { "mt_freeze.c", "perl_freeze_boot_image", "MT_FREEZE_FAILED", MT_FAULT_FATAL,
      "boot image freeze/seal failed; immutable invariant broken" },
    { "mt_freeze.c", "perl_freeze_boot_image", "MT_MY_CXT_WITHOUT_CLONE", MT_FAULT_RECOVERABLE,
      "XS module lacks CLONE method prior to freeze" },

    /* mt_worker.c */
    { "mt_worker.c", "Perl_mt_cv_depth_p", "MT_NOT_A_WORKER", MT_FAULT_RECOVERABLE,
      "worker CV depth table accessed outside worker" },
    { "mt_worker.c", "Perl_mt_padlist_p", "MT_NOT_A_WORKER", MT_FAULT_RECOVERABLE,
      "worker padlist table accessed outside worker" },
    { "mt_worker.c", "Perl_mt_hv_common_immortal", "MT_IMMORTAL_MUTATION", MT_FAULT_RECOVERABLE,
      "attempted mutation of sealed immortal hash or stash" },
    { "mt_worker.c", "Perl_mt_value_from_sv", "MT_UNSHAREABLE_REF", MT_FAULT_RECOVERABLE,
      "attempted transfer of mutable reference across workers" },
    { "mt_worker.c", "Perl_mt_worker_spawn", "MT_NOT_FROZEN/MT_WORKER_FAILED", MT_FAULT_RECOVERABLE,
      "worker spawn before freeze or thread creation failure" },
    { "mt_worker.c", "S_mt_worker_main", "MT_WORKER_FAILED", MT_FAULT_RECOVERABLE,
      "worker entry sub missing, exit() called, or uncaught die" },
    { "mt_worker.c", "XS_MT_Ctx_selftest", "MT_CTX_CLOBBERED", MT_FAULT_FATAL,
      "context switch clobbered callee-saved register" },

    /* mt_xs_bridge.c */
    { "mt_xs_bridge.c", "S_enc_sv", "MT_REMOTE_UNSUPPORTED_TYPE", MT_FAULT_RECOVERABLE,
      "unmarshallable SV type sent to XS compartment" },
    { "mt_xs_bridge.c", "S_enc_sv", "MT_STALE_REMOTE_HANDLE/MT_REMOTE_HANDLE_AFFINITY", MT_FAULT_RECOVERABLE,
      "stale or cross-compartment remote handle" },
    { "mt_xs_bridge.c", "S_check_magic", "MT_COMPARTMENT_MAGIC_ON_HOST_SV", MT_FAULT_RECOVERABLE,
      "compartment attached magic to host SV" },
    { "mt_xs_bridge.c", "S_remote_call", "MT_COMPARTMENT_DEAD", MT_FAULT_RECOVERABLE,
      "XS compartment process died while serving call" },
    { "mt_xs_bridge.c", "S_remote_call", "MT_XS_TOO_MANY_INFLIGHT", MT_FAULT_RECOVERABLE,
      "in-flight remote XS call limit exceeded" },

    /* mt_xs_compartment.c */
    { "mt_xs_compartment.c", "Perl_mt_xs_slot", "MT_NO_COMPARTMENT", MT_FAULT_RECOVERABLE,
      "no XS compartment pool or bridge slot available" }
};

void
Perl_mt_fault_init(void)
{
    Perl_mt_sup_init();
}

const char *
Perl_mt_fault_cause_name(uint32_t cause)
{
    switch (cause) {
    case MT_FAULT_NONE:             return "NONE";
    case MT_FAULT_ERR_CROAK:        return "ERR_CROAK";
    case MT_FAULT_HEARTBEAT_TIMEOUT:return "HEARTBEAT_TIMEOUT";
    case MT_FAULT_SIGNAL:           return "SIGNAL";
    case MT_FAULT_COMPARTMENT_CRASH:return "COMPARTMENT_CRASH";
    case MT_FAULT_WORKER_EXIT:      return "WORKER_EXIT";
    case MT_FAULT_CAP_EXHAUSTED:    return "CAP_EXHAUSTED";
    case MT_FAULT_INVARIANT:        return "INVARIANT";
    case MT_FAULT_OOM:              return "OOM";
    case MT_FAULT_TEST_RECOVERABLE: return "TEST_RECOVERABLE";
    case MT_FAULT_TEST_FATAL:       return "TEST_FATAL";
    default:                        return "UNKNOWN";
    }
}

const char *
Perl_mt_fault_severity_name(uint32_t severity)
{
    switch (severity) {
    case MT_FAULT_RECOVERABLE: return "recoverable";
    case MT_FAULT_FATAL:       return "fatal";
    default:                   return "unknown";
    }
}

const char *
Perl_mt_fault_unit_kind_name(uint32_t unit_kind)
{
    switch (unit_kind) {
    case MT_UNIT_MAIN:        return "main";
    case MT_UNIT_WORKER:      return "worker";
    case MT_UNIT_COMPARTMENT: return "compartment";
    default:                  return "unknown";
    }
}

const perl_mt_fault_site_t *
Perl_mt_fault_sites(size_t *count)
{
    if (count)
        *count = sizeof(S_fault_sites) / sizeof(S_fault_sites[0]);
    return S_fault_sites;
}

uint64_t
Perl_mt_fault_record(uint32_t cause, uint32_t severity,
                     uint32_t unit_kind, uint32_t unit_id,
                     uint32_t generation, int32_t err_code,
                     const char *msg)
{
    perl_mt_fault_t rec;
    uint64_t seq;

    memset(&rec, 0, sizeof rec);
    rec.timestamp_ns = perl_krt_now_ns();
    rec.cause        = cause;
    rec.severity     = severity;
    rec.unit_kind    = unit_kind;
    rec.unit_id      = unit_id;
    rec.generation   = generation;
    rec.err_code     = err_code;
    if (msg && *msg) {
        size_t len = strlen(msg);
        if (len >= MT_FAULT_MSG_MAX)
            len = MT_FAULT_MSG_MAX - 1;
        memcpy(rec.message, msg, len);
        rec.message[len] = '\0';
    }

    pthread_mutex_lock(&S_fault_lock);
    seq = ++S_fault_seq;
    rec.seq = seq;
    S_fault_last = rec;
    S_fault_ring[(seq - 1) % MT_FAULT_RING_CAP] = rec;
    pthread_mutex_unlock(&S_fault_lock);
    return seq;
}

int
Perl_mt_fault_last(perl_mt_fault_t *out)
{
    int have = 0;
    pthread_mutex_lock(&S_fault_lock);
    if (S_fault_seq > 0) {
        if (out)
            *out = S_fault_last;
        have = 1;
    }
    pthread_mutex_unlock(&S_fault_lock);
    return have;
}

uint64_t
Perl_mt_fault_count(void)
{
    uint64_t n;
    pthread_mutex_lock(&S_fault_lock);
    n = S_fault_seq;
    pthread_mutex_unlock(&S_fault_lock);
    return n;
}

void
Perl_mt_fault_reset(void)
{
    pthread_mutex_lock(&S_fault_lock);
    S_fault_seq = 0;
    memset(&S_fault_last, 0, sizeof S_fault_last);
    memset(S_fault_ring, 0, sizeof S_fault_ring);
    pthread_mutex_unlock(&S_fault_lock);
}

int
Perl_mt_fault_drain_events(uint64_t *after_seq, perl_mt_fault_t *buf,
                           int max_events)
{
    uint64_t start, latest, i;
    int n = 0;

    if (!after_seq || !buf || max_events <= 0)
        return 0;

    pthread_mutex_lock(&S_fault_lock);
    latest = S_fault_seq;
    start = *after_seq + 1;
    if (latest > MT_FAULT_RING_CAP && start <= latest - MT_FAULT_RING_CAP)
        start = latest - MT_FAULT_RING_CAP + 1;
    for (i = start; i <= latest && n < max_events; i++) {
        buf[n++] = S_fault_ring[(i - 1) % MT_FAULT_RING_CAP];
        *after_seq = i;
    }
    pthread_mutex_unlock(&S_fault_lock);
    return n;
}

void
Perl_mt_fault_raise(pTHX_ uint32_t cause, uint32_t severity,
                    uint32_t unit_kind, uint32_t unit_id,
                    uint32_t generation, int32_t err_code,
                    const char *msg)
{
    const char *text = (msg && *msg) ? msg : Perl_mt_fault_cause_name(cause);
    uint64_t seq = Perl_mt_fault_record(cause, severity, unit_kind,
                                        unit_id, generation, err_code, text);

    if (severity == MT_FAULT_FATAL) {
        char buf[MT_FAULT_MSG_MAX + 128];
        int n = snprintf(buf, sizeof buf,
                         "perl5-mt: FATAL fault #%llu [%s] unit=%s:%u gen=%u: %s\n",
                         (unsigned long long)seq,
                         Perl_mt_fault_cause_name(cause),
                         Perl_mt_fault_unit_kind_name(unit_kind),
                         (unsigned)unit_id,
                         (unsigned)generation,
                         text);
        if (n > 0) {
            size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
            ssize_t ignored = write(STDERR_FILENO, buf, len);
            (void)ignored;
        }
        _exit(134);
    }

    Perl_croak(aTHX_ "perl5-mt: fault #%llu [%s] unit=%s:%u gen=%u: %s",
               (unsigned long long)seq,
               Perl_mt_fault_cause_name(cause),
               Perl_mt_fault_unit_kind_name(unit_kind),
               (unsigned)unit_id,
               (unsigned)generation,
               text);
}

/* ---- supervisor heartbeats over krt_vec.h --------------------- */

typedef struct mt_sup_unit {
    uint32_t        state;         /* MT_SUP_STATE_*                        */
    uint32_t        unit_kind;     /* MT_UNIT_WORKER / MT_UNIT_COMPARTMENT  */
    uint32_t        unit_id;
    uint32_t        generation;
    uint64_t        last_beat_ns;
    uint32_t        missed_beats;
    perl_krt_au32_t hb_paused;     /* test hook: stop posting beats         */
    perl_krt_au64_t beat_count;
} mt_sup_unit_t;

typedef struct mt_sup_state {
    int              inited;
    perl_krt_shm_t   shm;
    perl_krt_vectab_t *vectab;
    int              shm_backed;
    uint32_t         hb_ms;        /* PERL_MT_HEARTBEAT_MS (default 100)    */
    uint32_t         hb_miss;      /* PERL_MT_HEARTBEAT_MISS (default 3)    */
    uint32_t         scan_word;    /* next PBA word to scan                 */
    uint32_t         high_slot;    /* highest slot ever allocated + 1       */
    mt_sup_unit_t    units[MT_SUP_MAX_UNITS];
} mt_sup_state_t;

static pthread_mutex_t S_sup_lock = PTHREAD_MUTEX_INITIALIZER;
static mt_sup_state_t  S_sup;

static _Thread_local int32_t  S_tls_sup_slot = -1;
static _Thread_local uint64_t S_tls_last_post_ns = 0;

static uint32_t
S_env_u32(const char *name, uint32_t defval, uint32_t minval)
{
    const char *e = getenv(name);
    char *end = NULL;
    unsigned long v;
    if (!e || !*e)
        return defval;
    v = strtoul(e, &end, 10);
    if (end == e || *end != '\0' || v < minval)
        return defval;
    return (uint32_t)v;
}

void
Perl_mt_sup_init(void)
{
    size_t sz;
    void *mem = NULL;

    pthread_mutex_lock(&S_sup_lock);
    if (S_sup.inited) {
        pthread_mutex_unlock(&S_sup_lock);
        return;
    }

    S_sup.hb_ms   = S_env_u32("PERL_MT_HEARTBEAT_MS", MT_SUP_DEFAULT_HB_MS, 5u);
    S_sup.hb_miss = S_env_u32("PERL_MT_HEARTBEAT_MISS", MT_SUP_DEFAULT_MISS, 1u);
    S_sup.scan_word = 0;
    S_sup.high_slot = 0;

    sz = perl_krt_vectab_size(MT_SUP_MAX_UNITS);
    if (perl_krt_shm_create(&S_sup.shm, sz, 0) == 0) {
        mem = S_sup.shm.base;
        S_sup.shm_backed = 1;
    }
    else {
        memset(&S_sup.shm, 0, sizeof S_sup.shm);
        mem = PerlMemShared_calloc(1, sz);
        S_sup.shm_backed = 0;
    }
    S_sup.vectab = perl_krt_vectab_init(mem, sz, MT_SUP_MAX_UNITS);
    memset(S_sup.units, 0, sizeof S_sup.units);
    S_sup.inited = 1;
    pthread_mutex_unlock(&S_sup_lock);
}

void
Perl_mt_sup_config(uint32_t *hb_ms, uint32_t *hb_miss)
{
    Perl_mt_sup_init();
    if (hb_ms)
        *hb_ms = S_sup.hb_ms;
    if (hb_miss)
        *hb_miss = S_sup.hb_miss;
}

int32_t
Perl_mt_sup_register(uint32_t unit_kind, uint32_t unit_id, uint32_t generation)
{
    uint32_t i;
    int32_t slot = -1;
    uint64_t now;

    Perl_mt_sup_init();
    now = perl_krt_now_ns();

    pthread_mutex_lock(&S_sup_lock);
    for (i = 0; i < MT_SUP_MAX_UNITS; i++) {
        if (S_sup.units[i].state == MT_SUP_STATE_FREE
            || S_sup.units[i].state == MT_SUP_STATE_EXITED) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot >= 0) {
        mt_sup_unit_t *u = &S_sup.units[slot];
        u->state        = MT_SUP_STATE_LIVE;
        u->unit_kind    = unit_kind;
        u->unit_id      = unit_id;
        u->generation   = generation;
        u->last_beat_ns = now;
        u->missed_beats = 0;
        atomic_init(&u->hb_paused, 0u);
        atomic_init(&u->beat_count, 0ull);
        if ((uint32_t)slot + 1 > S_sup.high_slot)
            S_sup.high_slot = (uint32_t)slot + 1;
    }
    pthread_mutex_unlock(&S_sup_lock);

    if (slot >= 0)
        Perl_mt_sup_beat_slot(slot);
    return slot;
}

void
Perl_mt_sup_unregister(int32_t slot)
{
    if (slot < 0 || (uint32_t)slot >= MT_SUP_MAX_UNITS)
        return;
    pthread_mutex_lock(&S_sup_lock);
    if (S_sup.units[slot].state == MT_SUP_STATE_LIVE)
        S_sup.units[slot].state = MT_SUP_STATE_EXITED;
    pthread_mutex_unlock(&S_sup_lock);
}

void
Perl_mt_sup_set_tls_slot(int32_t slot)
{
    S_tls_sup_slot = slot;
    S_tls_last_post_ns = perl_krt_now_ns();
}

int32_t
Perl_mt_sup_get_tls_slot(void)
{
    return S_tls_sup_slot;
}

void
Perl_mt_sup_pause_heartbeat(int32_t slot, int paused)
{
    if (slot < 0 || (uint32_t)slot >= MT_SUP_MAX_UNITS)
        return;
    perl_krt_store_seq_cst(&S_sup.units[slot].hb_paused, paused ? 1u : 0u);
}

int32_t
Perl_mt_sup_find_slot(uint32_t unit_kind, uint32_t unit_id)
{
    uint32_t i;
    int32_t found = -1;
    Perl_mt_sup_init();
    pthread_mutex_lock(&S_sup_lock);
    for (i = 0; i < S_sup.high_slot; i++) {
        if (S_sup.units[i].state != MT_SUP_STATE_FREE
            && S_sup.units[i].unit_kind == unit_kind
            && S_sup.units[i].unit_id == unit_id) {
            found = (int32_t)i;
            break;
        }
    }
    pthread_mutex_unlock(&S_sup_lock);
    return found;
}

/* Hot path: one atomic store (fetch_or) to the liveness vector's pending-bit
 * word (krt_vec.h).  No locks, no syscalls. */
void
Perl_mt_sup_beat_slot(int32_t slot)
{
    perl_krt_pba_word_t *w;
    uint64_t bit;

    if (UNLIKELY(!S_sup.inited || !S_sup.vectab
                 || slot < 0 || (uint32_t)slot >= MT_SUP_MAX_UNITS))
        return;
    if (UNLIKELY(perl_krt_load_relaxed(&S_sup.units[slot].hb_paused) != 0))
        return;

    w = perl_krt_pba_at(S_sup.vectab, (uint32_t)slot / PERL_KRT_PBA_BITS);
    bit = (uint64_t)1 << ((uint32_t)slot % PERL_KRT_PBA_BITS);
    atomic_fetch_or_explicit(&w->bits, bit, memory_order_release);
    atomic_fetch_add_explicit(&S_sup.units[slot].beat_count, 1ull,
                              memory_order_relaxed);
}

void
Perl_mt_sup_maybe_beat(void)
{
    int32_t slot = S_tls_sup_slot;
    uint64_t now, interval_ns;

    if (slot < 0 || !S_sup.inited)
        return;
    if (UNLIKELY(perl_krt_load_relaxed(&S_sup.units[slot].hb_paused) != 0))
        return;
    now = perl_krt_now_ns();
    interval_ns = (uint64_t)S_sup.hb_ms * 1000000ull / 2ull;
    if (interval_ns < 1000000ull)
        interval_ns = 1000000ull;
    if (now - S_tls_last_post_ns >= interval_ns) {
        S_tls_last_post_ns = now;
        Perl_mt_sup_beat_slot(slot);
    }
}

uint64_t
Perl_mt_sup_bench_beat(int32_t slot, uint64_t iters)
{
    uint64_t t0, t1, i;
    perl_krt_pba_word_t *w;
    uint64_t bit;

    Perl_mt_sup_init();
    if (slot < 0 || (uint32_t)slot >= MT_SUP_MAX_UNITS)
        slot = 0;
    w = perl_krt_pba_at(S_sup.vectab, (uint32_t)slot / PERL_KRT_PBA_BITS);
    bit = (uint64_t)1 << ((uint32_t)slot % PERL_KRT_PBA_BITS);

    t0 = perl_krt_now_ns();
    for (i = 0; i < iters; i++) {
        atomic_fetch_or_explicit(&w->bits, bit, memory_order_release);
    }
    t1 = perl_krt_now_ns();
    return t1 - t0;
}

/* Main interpreter supervisor liveness scan: scans ONE pending-bit word per
 * tick and marks any live unit that missed >= PERL_MT_HEARTBEAT_MISS beats
 * as FAULTED, recording a fault event. */
U32
Perl_mt_sup_tick(pTHX)
{
    uint32_t nwords, word, base, end, i;
    uint64_t bits, now, hb_ns;
    U32 newly_faulted = 0;

    PERL_UNUSED_CONTEXT;
    Perl_mt_sup_init();

    pthread_mutex_lock(&S_sup_lock);
    if (S_sup.high_slot == 0) {
        pthread_mutex_unlock(&S_sup_lock);
        return 0;
    }
    nwords = (S_sup.high_slot + PERL_KRT_PBA_BITS - 1) / PERL_KRT_PBA_BITS;
    word = S_sup.scan_word % nwords;
    S_sup.scan_word++;

    bits = perl_krt_pba_take(S_sup.vectab, word);
    now  = perl_krt_now_ns();
    hb_ns = (uint64_t)S_sup.hb_ms * 1000000ull;

    base = word * PERL_KRT_PBA_BITS;
    end  = base + PERL_KRT_PBA_BITS;
    if (end > S_sup.high_slot)
        end = S_sup.high_slot;

    for (i = base; i < end; i++) {
        mt_sup_unit_t *u = &S_sup.units[i];
        uint64_t mask = (uint64_t)1 << (i - base);

        if (u->state != MT_SUP_STATE_LIVE)
            continue;

        if (bits & mask) {
            u->last_beat_ns = now;
            u->missed_beats = 0;
        }
        else if (now > u->last_beat_ns) {
            uint64_t elapsed = now - u->last_beat_ns;
            uint32_t missed = (uint32_t)(elapsed / hb_ns);
            u->missed_beats = missed;
            if (missed >= S_sup.hb_miss) {
                char msg[MT_FAULT_MSG_MAX];
                u->state = MT_SUP_STATE_FAULTED;
                snprintf(msg, sizeof msg,
                         "%s %u missed %u heartbeats (%llu ms > %u * %u ms)",
                         Perl_mt_fault_unit_kind_name(u->unit_kind),
                         (unsigned)u->unit_id,
                         (unsigned)missed,
                         (unsigned long long)(elapsed / 1000000ull),
                         (unsigned)S_sup.hb_miss,
                         (unsigned)S_sup.hb_ms);
                Perl_mt_fault_record(MT_FAULT_HEARTBEAT_TIMEOUT,
                                     MT_FAULT_RECOVERABLE,
                                     u->unit_kind,
                                     u->unit_id,
                                     u->generation,
                                     MT_WORKER_FAILED,
                                     msg);
                newly_faulted++;
            }
        }
    }
    pthread_mutex_unlock(&S_sup_lock);
    return newly_faulted;
}

size_t
Perl_mt_sup_snapshot(perl_mt_sup_unit_info_t *out, size_t max_units)
{
    uint32_t i;
    size_t n = 0;
    uint64_t now;

    Perl_mt_sup_init();
    now = perl_krt_now_ns();

    pthread_mutex_lock(&S_sup_lock);
    for (i = 0; i < S_sup.high_slot && n < max_units; i++) {
        mt_sup_unit_t *u = &S_sup.units[i];
        if (u->state == MT_SUP_STATE_FREE)
            continue;
        out[n].slot         = i;
        out[n].unit_kind    = u->unit_kind;
        out[n].unit_id      = u->unit_id;
        out[n].generation   = u->generation;
        out[n].state        = u->state;
        out[n].missed_beats = u->missed_beats;
        out[n].last_beat_ns = u->last_beat_ns;
        out[n].age_ms       = now > u->last_beat_ns
                            ? (now - u->last_beat_ns) / 1000000ull : 0;
        out[n].beat_count   = atomic_load_explicit(&u->beat_count,
                                                   memory_order_relaxed);
        n++;
    }
    pthread_mutex_unlock(&S_sup_lock);
    return n;
}

/* ---- Perl SV helpers for ext/MT/mt_phase6.xs.inc ------------------------ */

static HV *
S_fault_to_hv(pTHX_ const perl_mt_fault_t *f)
{
    HV *hv = newHV();
    (void)hv_stores(hv, "seq",          newSVuv((UV)f->seq));
    (void)hv_stores(hv, "timestamp_ns", newSVuv((UV)f->timestamp_ns));
    (void)hv_stores(hv, "cause",        newSVpv(Perl_mt_fault_cause_name(f->cause), 0));
    (void)hv_stores(hv, "cause_code",   newSVuv((UV)f->cause));
    (void)hv_stores(hv, "severity",     newSVpv(Perl_mt_fault_severity_name(f->severity), 0));
    (void)hv_stores(hv, "unit_kind",    newSVpv(Perl_mt_fault_unit_kind_name(f->unit_kind), 0));
    (void)hv_stores(hv, "unit_id",      newSVuv((UV)f->unit_id));
    (void)hv_stores(hv, "generation",   newSVuv((UV)f->generation));
    (void)hv_stores(hv, "err_code",     newSViv((IV)f->err_code));
    (void)hv_stores(hv, "message",      newSVpv(f->message, 0));
    return hv;
}

SV *
Perl_mt_fault_last_sv(pTHX)
{
    perl_mt_fault_t f;
    if (!Perl_mt_fault_last(&f))
        return newSV(0);
    return newRV_noinc((SV *)S_fault_to_hv(aTHX_ &f));
}

AV *
Perl_mt_fault_drain_av(pTHX_ uint64_t *after_seq)
{
    perl_mt_fault_t buf[MT_FAULT_RING_CAP];
    int i, n;
    AV *av = newAV();

    n = Perl_mt_fault_drain_events(after_seq, buf, (int)MT_FAULT_RING_CAP);
    for (i = 0; i < n; i++)
        av_push(av, newRV_noinc((SV *)S_fault_to_hv(aTHX_ &buf[i])));
    return av;
}

AV *
Perl_mt_fault_sites_av(pTHX)
{
    size_t i, n = 0;
    const perl_mt_fault_site_t *sites = Perl_mt_fault_sites(&n);
    AV *av = newAV();

    for (i = 0; i < n; i++) {
        HV *hv = newHV();
        (void)hv_stores(hv, "file",      newSVpv(sites[i].file, 0));
        (void)hv_stores(hv, "function",  newSVpv(sites[i].function, 0));
        (void)hv_stores(hv, "err_name",  newSVpv(sites[i].err_name, 0));
        (void)hv_stores(hv, "severity",  newSVpv(Perl_mt_fault_severity_name(sites[i].severity), 0));
        (void)hv_stores(hv, "rationale", newSVpv(sites[i].rationale, 0));
        av_push(av, newRV_noinc((SV *)hv));
    }
    return av;
}

static const char *
S_sup_state_name(uint32_t st)
{
    switch (st) {
    case MT_SUP_STATE_LIVE:    return "live";
    case MT_SUP_STATE_FAULTED: return "faulted";
    case MT_SUP_STATE_EXITED:  return "exited";
    default:                   return "free";
    }
}

AV *
Perl_mt_sup_status_av(pTHX)
{
    perl_mt_sup_unit_info_t info[MT_SUP_MAX_UNITS];
    size_t i, n = Perl_mt_sup_snapshot(info, MT_SUP_MAX_UNITS);
    AV *av = newAV();

    for (i = 0; i < n; i++) {
        HV *hv = newHV();
        char unit_label[64];
        snprintf(unit_label, sizeof unit_label, "%s:%u",
                 Perl_mt_fault_unit_kind_name(info[i].unit_kind),
                 (unsigned)info[i].unit_id);
        (void)hv_stores(hv, "slot",         newSVuv((UV)info[i].slot));
        (void)hv_stores(hv, "unit",         newSVpv(unit_label, 0));
        (void)hv_stores(hv, "unit_kind",    newSVpv(Perl_mt_fault_unit_kind_name(info[i].unit_kind), 0));
        (void)hv_stores(hv, "unit_id",      newSVuv((UV)info[i].unit_id));
        (void)hv_stores(hv, "generation",   newSVuv((UV)info[i].generation));
        (void)hv_stores(hv, "last_beat_ns", newSVuv((UV)info[i].last_beat_ns));
        (void)hv_stores(hv, "age_ms",       newSVuv((UV)info[i].age_ms));
        (void)hv_stores(hv, "missed_beats", newSVuv((UV)info[i].missed_beats));
        (void)hv_stores(hv, "beat_count",   newSVuv((UV)info[i].beat_count));
        (void)hv_stores(hv, "state",        newSVpv(S_sup_state_name(info[i].state), 0));
        av_push(av, newRV_noinc((SV *)hv));
    }
    return av;
}

/* ---- MT::Pool nested data & capability marshaler ----------- */

#define MT_POOL_MAX_DEPTH 64

typedef struct mt_pool_buf {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} mt_pool_buf_t;

static void
S_pbuf_reserve(mt_pool_buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        while (ncap < b->len + extra)
            ncap *= 2;
        Renew(b->data, ncap, unsigned char);
        b->cap = ncap;
    }
}

static void
S_pbuf_put(mt_pool_buf_t *b, const void *src, size_t n)
{
    S_pbuf_reserve(b, n);
    if (n)
        memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void
S_pbuf_put_u8(mt_pool_buf_t *b, uint8_t v)
{
    S_pbuf_put(b, &v, 1);
}

static void
S_pbuf_put_u32(mt_pool_buf_t *b, uint32_t v)
{
    S_pbuf_put(b, &v, sizeof v);
}

static void
S_pool_validate_sv(pTHX_ SV *sv, int depth)
{
    if (depth > MT_POOL_MAX_DEPTH)
        Perl_croak_mt(aTHX_ MT_UNSHAREABLE_REF);
    if (!sv || !SvOK(sv))
        return;
    if (Perl_mt_cap_is_handle(aTHX_ sv))
        return;
    if (SvROK(sv)) {
        SV *rv = SvRV(sv);
        svtype t = SvTYPE(rv);
        if (SvOBJECT(rv))
            Perl_croak_mt(aTHX_ MT_UNSHAREABLE_REF);
        if (t == SVt_PVAV) {
            AV *av = (AV *)rv;
            SSize_t i, n = av_len(av) + 1;
            for (i = 0; i < n; i++) {
                SV **elem = av_fetch(av, i, 0);
                S_pool_validate_sv(aTHX_ elem ? *elem : NULL, depth + 1);
            }
            return;
        }
        if (t == SVt_PVHV) {
            HV *hv = (HV *)rv;
            HE *he;
            hv_iterinit(hv);
            while ((he = hv_iternext(hv)) != NULL)
                S_pool_validate_sv(aTHX_ hv_iterval(hv, he), depth + 1);
            return;
        }
        if (t <= SVt_PVMG) {
            S_pool_validate_sv(aTHX_ rv, depth + 1);
            return;
        }
        Perl_croak_mt(aTHX_ MT_UNSHAREABLE_REF);
    }
}

static void
S_pool_encode_sv(pTHX_ mt_pool_buf_t *b, SV *sv)
{
    if (!sv || !SvOK(sv)) {
        S_pbuf_put_u8(b, 'u');
        return;
    }
    if (Perl_mt_cap_is_handle(aTHX_ sv)) {
        U32 slot = 0, gen = 0;
        Perl_mt_cap_share(aTHX_ sv, &slot, &gen);
        S_pbuf_put_u8(b, 'c');
        S_pbuf_put_u32(b, (uint32_t)slot);
        S_pbuf_put_u32(b, (uint32_t)gen);
        return;
    }
    if (SvROK(sv)) {
        SV *rv = SvRV(sv);
        svtype t = SvTYPE(rv);
        if (t == SVt_PVAV) {
            AV *av = (AV *)rv;
            SSize_t i, n = av_len(av) + 1;
            S_pbuf_put_u8(b, 'a');
            S_pbuf_put_u32(b, (uint32_t)n);
            for (i = 0; i < n; i++) {
                SV **elem = av_fetch(av, i, 0);
                S_pool_encode_sv(aTHX_ b, elem ? *elem : NULL);
            }
            return;
        }
        if (t == SVt_PVHV) {
            HV *hv = (HV *)rv;
            uint32_t nkeys = (uint32_t)HvUSEDKEYS(hv);
            HE *he;
            S_pbuf_put_u8(b, 'h');
            S_pbuf_put_u32(b, nkeys);
            hv_iterinit(hv);
            while ((he = hv_iternext(hv)) != NULL) {
                STRLEN klen = 0;
                const char *k = HePV(he, klen);
                uint8_t kutf8 = HeUTF8(he) ? 1u : 0u;
                S_pbuf_put_u32(b, (uint32_t)klen);
                S_pbuf_put_u8(b, kutf8);
                S_pbuf_put(b, k, (size_t)klen);
                S_pool_encode_sv(aTHX_ b, hv_iterval(hv, he));
            }
            return;
        }
        S_pbuf_put_u8(b, 'r');
        S_pool_encode_sv(aTHX_ b, rv);
        return;
    }
    if (SvIOK(sv) && !SvPOK(sv)) {
        IV iv = SvIV(sv);
        S_pbuf_put_u8(b, 'i');
        S_pbuf_put(b, &iv, sizeof iv);
        return;
    }
    if (SvNOK(sv) && !SvPOK(sv)) {
        NV nv = SvNV(sv);
        S_pbuf_put_u8(b, 'n');
        S_pbuf_put(b, &nv, sizeof nv);
        return;
    }
    {
        STRLEN len = 0;
        const char *pv = SvPV_const(sv, len);
        uint8_t utf8 = SvUTF8(sv) ? 1u : 0u;
        S_pbuf_put_u8(b, 'p');
        S_pbuf_put_u32(b, (uint32_t)len);
        S_pbuf_put_u8(b, utf8);
        S_pbuf_put(b, pv, (size_t)len);
    }
}

SV *
Perl_mt_pool_encode_list(pTHX_ SV **items, I32 count)
{
    mt_pool_buf_t b;
    I32 i;
    SV *out;

    /* Validate everything before sharing any capabilities. */
    for (i = 0; i < count; i++)
        S_pool_validate_sv(aTHX_ items[i], 0);

    b.data = NULL;
    b.len  = 0;
    b.cap  = 0;
    S_pbuf_put(&b, "MTP1", 4);
    S_pbuf_put_u32(&b, (uint32_t)count);
    for (i = 0; i < count; i++)
        S_pool_encode_sv(aTHX_ &b, items[i]);

    out = newSVpvn((const char *)b.data, (STRLEN)b.len);
    Safefree(b.data);
    return out;
}

static int
S_pbuf_get(const unsigned char **p, const unsigned char *end,
           void *dst, size_t n)
{
    if ((size_t)(end - *p) < n)
        return 0;
    if (n)
        memcpy(dst, *p, n);
    *p += n;
    return 1;
}

static SV *
S_pool_decode_sv(pTHX_ const unsigned char **p, const unsigned char *end,
                 int depth)
{
    uint8_t tag = 0;
    if (depth > MT_POOL_MAX_DEPTH || !S_pbuf_get(p, end, &tag, 1))
        Perl_croak(aTHX_ "MT::Pool: corrupt encoded message");

    switch (tag) {
    case 'u':
        return newSV(0);
    case 'i': {
        IV iv;
        if (!S_pbuf_get(p, end, &iv, sizeof iv))
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded IV");
        return newSViv(iv);
    }
    case 'n': {
        NV nv;
        if (!S_pbuf_get(p, end, &nv, sizeof nv))
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded NV");
        return newSVnv(nv);
    }
    case 'p': {
        uint32_t len = 0;
        uint8_t utf8 = 0;
        SV *sv;
        if (!S_pbuf_get(p, end, &len, sizeof len)
            || !S_pbuf_get(p, end, &utf8, 1)
            || (size_t)(end - *p) < (size_t)len)
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded PV");
        sv = newSVpvn((const char *)*p, (STRLEN)len);
        *p += len;
        if (utf8)
            SvUTF8_on(sv);
        return sv;
    }
    case 'c': {
        uint32_t slot = 0, gen = 0;
        if (!S_pbuf_get(p, end, &slot, sizeof slot)
            || !S_pbuf_get(p, end, &gen, sizeof gen))
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded capability");
        return Perl_mt_cap_handle_new(aTHX_ (U32)slot, (U32)gen);
    }
    case 'a': {
        uint32_t i, n = 0;
        AV *av;
        if (!S_pbuf_get(p, end, &n, sizeof n))
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded array");
        av = newAV();
        if (n)
            av_extend(av, (SSize_t)n - 1);
        for (i = 0; i < n; i++)
            av_push(av, S_pool_decode_sv(aTHX_ p, end, depth + 1));
        return newRV_noinc((SV *)av);
    }
    case 'h': {
        uint32_t i, n = 0;
        HV *hv;
        if (!S_pbuf_get(p, end, &n, sizeof n))
            Perl_croak(aTHX_ "MT::Pool: corrupt encoded hash");
        hv = newHV();
        for (i = 0; i < n; i++) {
            uint32_t klen = 0;
            uint8_t kutf8 = 0;
            const char *k;
            SV *val;
            if (!S_pbuf_get(p, end, &klen, sizeof klen)
                || !S_pbuf_get(p, end, &kutf8, 1)
                || (size_t)(end - *p) < (size_t)klen)
                Perl_croak(aTHX_ "MT::Pool: corrupt encoded hash key");
            k = (const char *)*p;
            *p += klen;
            val = S_pool_decode_sv(aTHX_ p, end, depth + 1);
            (void)hv_store(hv, k, kutf8 ? -(I32)klen : (I32)klen, val, 0);
        }
        return newRV_noinc((SV *)hv);
    }
    case 'r': {
        SV *inner = S_pool_decode_sv(aTHX_ p, end, depth + 1);
        return newRV_noinc(inner);
    }
    default:
        Perl_croak(aTHX_ "MT::Pool: unknown encoded tag 0x%02x", (unsigned)tag);
    }
    return newSV(0);
}

AV *
Perl_mt_pool_decode_list(pTHX_ SV *blob)
{
    STRLEN len = 0;
    const unsigned char *p = (const unsigned char *)SvPV_const(blob, len);
    const unsigned char *end = p + len;
    uint32_t i, count = 0;
    AV *av;

    if (len < 8 || memcmp(p, "MTP1", 4) != 0)
        Perl_croak(aTHX_ "MT::Pool: corrupt message header");
    p += 4;
    if (!S_pbuf_get(&p, end, &count, sizeof count))
        Perl_croak(aTHX_ "MT::Pool: corrupt message count");

    av = newAV();
    if (count)
        av_extend(av, (SSize_t)count - 1);
    for (i = 0; i < count; i++)
        av_push(av, S_pool_decode_sv(aTHX_ &p, end, 0));
    return av;
}

/* ---- combined aio + doorbell wait probe for tests ---------- */

int
Perl_mt_aio_test_combined_sleep(pTHX)
{
    perl_krt_aio_t ctx;
    perl_krt_au32_t db;
    int rc;

    PERL_UNUSED_CONTEXT;
    memset(&ctx, 0, sizeof ctx);
    atomic_init(&db, 1u);
    rc = perl_krt_aio_init(&ctx, 8, NULL);
    if (rc != 0)
        return rc;
    rc = perl_krt_aio_wait_doorbell(&ctx, (volatile uint32_t *)&db,
                                    1u, 0u, 1000000ull);
    (void)perl_krt_aio_close(&ctx);
    return rc;
}

/* ---- per-worker signal containment ---------------------------- */

static _Thread_local perl_mt_worker_sig_ctx_t S_worker_sig_ctx;
static pthread_once_t   S_sig_handlers_once = PTHREAD_ONCE_INIT;
static struct sigaction S_prev_sa_segv;
static struct sigaction S_prev_sa_bus;
static struct sigaction S_prev_sa_fpe;

const char *
Perl_mt_fault_signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    default:      return "SIGNAL";
    }
}

static void
S_mt_worker_sig_handler(int sig, siginfo_t *info, void *ucontext)
{
    (void)ucontext;
    if (S_worker_sig_ctx.armed) {
        S_worker_sig_ctx.armed = 0;
        S_worker_sig_ctx.caught_sig = sig;
        S_worker_sig_ctx.fault_addr = info ? (uintptr_t)info->si_addr : 0;
        S_worker_sig_ctx.fault_code = info ? info->si_code : 0;
        siglongjmp(S_worker_sig_ctx.jmp, sig ? sig : SIGSEGV);
    }

    /* Not an armed worker thread (e.g. main thread): chain to previous
     * handler or restore default so real host faults terminate fail-fast. */
    {
        struct sigaction *prev = NULL;
        if (sig == SIGSEGV)      prev = &S_prev_sa_segv;
        else if (sig == SIGBUS)  prev = &S_prev_sa_bus;
        else if (sig == SIGFPE)  prev = &S_prev_sa_fpe;

        if (prev && (prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
            prev->sa_sigaction(sig, info, ucontext);
            return;
        }
        if (prev && !(prev->sa_flags & SA_SIGINFO)
            && prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
            prev->sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void
S_install_sig_handlers_once(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = S_mt_worker_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

    (void)sigaction(SIGSEGV, &sa, &S_prev_sa_segv);
    (void)sigaction(SIGBUS,  &sa, &S_prev_sa_bus);
    (void)sigaction(SIGFPE,  &sa, &S_prev_sa_fpe);
}

void
Perl_mt_fault_install_signal_handlers(void)
{
    (void)pthread_once(&S_sig_handlers_once, S_install_sig_handlers_once);
}

perl_mt_worker_sig_ctx_t *
Perl_mt_fault_worker_sig_ctx(void)
{
    return &S_worker_sig_ctx;
}

int
Perl_mt_fault_altstack_setup(perl_mt_worker_sig_ctx_t *ctx)
{
    size_t sz;
    void *mem;
    stack_t ss;

    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof *ctx);
    sz = (size_t)SIGSTKSZ;
    if (sz < 65536u)
        sz = 65536u;
    mem = mmap(NULL, sz, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
        return -1;
    ss.ss_sp = mem;
    ss.ss_size = sz;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        munmap(mem, sz);
        return -1;
    }
    ctx->altstack_mem = mem;
    ctx->altstack_size = sz;
    ctx->altstack_installed = 1;
    return 0;
}

void
Perl_mt_fault_altstack_teardown(perl_mt_worker_sig_ctx_t *ctx)
{
    if (!ctx)
        return;
    ctx->armed = 0;
    if (ctx->altstack_installed) {
        stack_t ss;
        memset(&ss, 0, sizeof ss);
        ss.ss_flags = SS_DISABLE;
        (void)sigaltstack(&ss, NULL);
        ctx->altstack_installed = 0;
    }
    if (ctx->altstack_mem && ctx->altstack_size) {
        munmap(ctx->altstack_mem, ctx->altstack_size);
        ctx->altstack_mem = NULL;
        ctx->altstack_size = 0;
    }
}

void
Perl_mt_fault_trigger_sigsegv(void)
{
    volatile int *p = (volatile int *)NULL;
    volatile int v = *p;
    (void)v;
    (void)pthread_kill(pthread_self(), SIGSEGV);
}

void
Perl_mt_fault_trigger_sigbus(void)
{
    (void)pthread_kill(pthread_self(), SIGBUS);
}

void
Perl_mt_fault_trigger_sigfpe(void)
{
#if defined(__x86_64__) || defined(__i386__)
    volatile int zero = 0;
    volatile int v = 1 / zero;
    (void)v;
#endif
    (void)pthread_kill(pthread_self(), SIGFPE);
}

void
Perl_mt_fault_trigger_oom(pTHX)
{
    /* Request an impossible allocation so malloc returns NULL without
     * destabilizing sibling threads, then raise a recoverable OOM fault. */
    volatile size_t huge = ((size_t)-1) >> 2;
    void *p = malloc(huge);
    if (p)
        free(p);
    Perl_mt_fault_raise(aTHX_ MT_FAULT_OOM, MT_FAULT_RECOVERABLE,
                        PL_mt_worker_id ? MT_UNIT_WORKER : MT_UNIT_MAIN,
                        (uint32_t)PL_mt_worker_id, 1u, MT_WORKER_FAILED,
                        "MT_WORKER_FAULTED: worker out of memory (OOM)");
}

/* ---- process-wide named endpoint registry --------------------- */

#define MT_NAMED_MAX_ENTRIES 512u
#define MT_NAMED_KEY_MAX     128u

typedef struct mt_named_entry {
    uint32_t in_use;
    uint32_t kind;
    uint64_t id;
    char     name[MT_NAMED_KEY_MAX];
} mt_named_entry_t;

static pthread_mutex_t  S_named_lock = PTHREAD_MUTEX_INITIALIZER;
static mt_named_entry_t S_named_table[MT_NAMED_MAX_ENTRIES];

int
Perl_mt_named_register(uint32_t kind, const char *name, uint64_t id,
                       int overwrite, uint64_t *existing_id)
{
    uint32_t i;
    int free_idx = -1;
    size_t len;

    if (!name || !*name)
        return -1;
    len = strlen(name);
    if (len >= MT_NAMED_KEY_MAX)
        return -1;

    pthread_mutex_lock(&S_named_lock);
    for (i = 0; i < MT_NAMED_MAX_ENTRIES; i++) {
        if (S_named_table[i].in_use) {
            if (S_named_table[i].kind == kind
                && strcmp(S_named_table[i].name, name) == 0) {
                if (overwrite) {
                    S_named_table[i].id = id;
                    if (existing_id) *existing_id = id;
                    pthread_mutex_unlock(&S_named_lock);
                    return 0;
                }
                if (existing_id) *existing_id = S_named_table[i].id;
                pthread_mutex_unlock(&S_named_lock);
                return 1; /* already exists */
            }
        }
        else if (free_idx < 0) {
            free_idx = (int)i;
        }
    }
    if (free_idx < 0) {
        pthread_mutex_unlock(&S_named_lock);
        return -1;
    }
    S_named_table[free_idx].in_use = 1;
    S_named_table[free_idx].kind = kind;
    S_named_table[free_idx].id = id;
    memcpy(S_named_table[free_idx].name, name, len + 1);
    if (existing_id) *existing_id = id;
    pthread_mutex_unlock(&S_named_lock);
    return 0;
}

int
Perl_mt_named_lookup(uint32_t kind, const char *name, uint64_t *id_out)
{
    uint32_t i;
    if (!name || !*name)
        return 0;
    pthread_mutex_lock(&S_named_lock);
    for (i = 0; i < MT_NAMED_MAX_ENTRIES; i++) {
        if (S_named_table[i].in_use
            && S_named_table[i].kind == kind
            && strcmp(S_named_table[i].name, name) == 0) {
            if (id_out) *id_out = S_named_table[i].id;
            pthread_mutex_unlock(&S_named_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&S_named_lock);
    return 0;
}

int
Perl_mt_named_unregister(uint32_t kind, const char *name)
{
    uint32_t i;
    if (!name || !*name)
        return 0;
    pthread_mutex_lock(&S_named_lock);
    for (i = 0; i < MT_NAMED_MAX_ENTRIES; i++) {
        if (S_named_table[i].in_use
            && S_named_table[i].kind == kind
            && strcmp(S_named_table[i].name, name) == 0) {
            S_named_table[i].in_use = 0;
            pthread_mutex_unlock(&S_named_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&S_named_lock);
    return 0;
}

AV *
Perl_mt_named_list_av(pTHX_ uint32_t kind)
{
    AV *av = newAV();
    uint32_t i;
    pthread_mutex_lock(&S_named_lock);
    for (i = 0; i < MT_NAMED_MAX_ENTRIES; i++) {
        if (S_named_table[i].in_use && S_named_table[i].kind == kind)
            av_push(av, newSVpv(S_named_table[i].name, 0));
    }
    pthread_mutex_unlock(&S_named_lock);
    return av;
}

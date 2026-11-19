/*    mt_chan.c
 *    perl5-mt: channels, cells and shared hashes.  The state lives
 *    in process memory, which the worker threads share: a channel, a cell or
 *    a shared hash is named by its id (an index into one process-wide table),
 *    and an id can be passed to any worker as an ordinary integer.  The Perl
 *    classes (MT::Channel, MT::Cell, MT::SharedHash) are in ext/MT.
 *
 *    Values cross as perl_mt_value_t copies (Perl_mt_value_from_sv /
 *    Perl_mt_value_to_sv, the path the worker join uses): scalars, capabilities
 *    and shared memory pass; a reference to a mutable value croaks
 *    MT_UNSHAREABLE_REF.
 *
 *    Waiting.  A thread waits on its channel's sequence word (krt futex wait,
 *    2 ms at most per wait, then checks again).  Every change bumps the word,
 *    and wakes the waiters when there are any.  A scheduled fiber that waits
 *    parks, and is linked on PL_mt_chan_waiters.  The loop's poll and idle hook
 *    (mt_fiber.c) call Perl_mt_chan_drain, which rechecks the parked fibers'
 *    channels and wakes those that can go on; the idle hook waits on the
 *    channel word, 1 ms at most, so a change made by another worker reaches
 *    the parked fiber within a millisecond.
 *
 *    Cells are a value under a mutex (see the deviation note in MT_README.md:
 *    compare-and-swap is done under the cell's lock, not on the body pointer).
 *    Shared hashes are 256 shards of 64 buckets, each shard under its own
 *    mutex.  Nothing here is freed: a channel, cell or hash lives until the
 *    process exits.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_CHAN_C
#include "perl.h"
#include "XSUB.h"
#include "mt_fiber.h"
#include "mt_fault.h"
#include "krt/krt_vec.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MT_CHAN_THREAD_WAIT_NS  20000000ull  /* backstop for heartbeat check */
#define MT_CHAN_IDLE_WAIT_NS    20000000ull  /* slot-vector wake is immediate */
#define MT_CHAN_VEC_SLOTS       256u
#define MT_CHAN_VEC_WORDS       (MT_CHAN_VEC_SLOTS / PERL_KRT_PBA_BITS)
#define MT_SH_SHARDS            256
#define MT_SH_BUCKETS           64

#define MT_CHAN_KIND_CHANNEL  1
#define MT_CHAN_KIND_CELL     2
#define MT_CHAN_KIND_SHARED   3

/* the modes of a parked waiter */
#define MT_CHAN_MODE_RECV  1
#define MT_CHAN_MODE_SEND  2

/* ---- the object table ------------------------------------------------------- */

typedef struct mt_obj {
    int   kind;
    void *p;
} mt_obj_t;

static pthread_mutex_t S_obj_lock = PTHREAD_MUTEX_INITIALIZER;
static mt_obj_t *S_objs = NULL;
static UV S_nobjs = 0, S_capobjs = 0;

static UV
S_obj_add(int kind, void *p)
{
    UV id;
    pthread_mutex_lock(&S_obj_lock);
    if (S_nobjs == S_capobjs) {
        UV cap = S_capobjs ? S_capobjs * 2 : 64;
        mt_obj_t *n = (mt_obj_t *)realloc(S_objs, (size_t)cap * sizeof *n);
        if (!n) {
            pthread_mutex_unlock(&S_obj_lock);
            croak_no_mem();
        }
        S_objs = n;
        S_capobjs = cap;
    }
    S_objs[S_nobjs].kind = kind;
    S_objs[S_nobjs].p = p;
    id = ++S_nobjs;                    /* ids start at 1 */
    pthread_mutex_unlock(&S_obj_lock);
    return id;
}

static void *
S_obj_get(pTHX_ UV id, int kind)
{
    void *p = NULL;
    pthread_mutex_lock(&S_obj_lock);
    if (id >= 1 && id <= S_nobjs && S_objs[id - 1].kind == kind)
        p = S_objs[id - 1].p;
    pthread_mutex_unlock(&S_obj_lock);
    if (!p)
        Perl_croak_mt(aTHX_ MT_NO_SUCH_OBJECT);
    return p;
}

/* ---- channels (slot-vector cross-worker wake) --------------------------- */

static pthread_once_t    S_chan_vec_once = PTHREAD_ONCE_INIT;
static perl_krt_vectab_t *S_chan_vec = NULL;
static _Thread_local perl_krt_poll_policy_t S_chan_poll;
static _Thread_local int S_chan_poll_inited = 0;

static void
S_chan_vec_init_once(void)
{
    size_t sz = perl_krt_vectab_size(MT_CHAN_VEC_SLOTS);
    void *mem = calloc(1, sz);
    if (mem)
        S_chan_vec = perl_krt_vectab_init(mem, sz, MT_CHAN_VEC_SLOTS);
}

static inline perl_krt_vectab_t *
S_chan_vectab(void)
{
    pthread_once(&S_chan_vec_once, S_chan_vec_init_once);
    return S_chan_vec;
}

static inline perl_krt_poll_policy_t *
S_chan_policy(void)
{
    if (UNLIKELY(!S_chan_poll_inited)) {
        perl_krt_poll_policy_init(&S_chan_poll,
                                  PERL_KRT_POLL_DEFAULT_MIN_NS,
                                  PERL_KRT_POLL_DEFAULT_MAX_NS);
        S_chan_poll_inited = 1;
    }
    return &S_chan_poll;
}

static inline uint32_t
S_chan_worker_slot(pTHX)
{
    U32 wid = PL_mt_worker_id;
    if (wid == 0)
        return 0u;
    return 1u + ((wid - 1u) % (MT_CHAN_VEC_SLOTS - 1u));
}

typedef struct mt_chan {
    pthread_mutex_t lock;
    perl_krt_au32_t seq;      /* bumped on every change                      */
    perl_krt_au32_t nwait;    /* threads and parked fibers waiting on it     */
    perl_krt_au64_t waiter_bits[MT_CHAN_VEC_WORDS]; /* waiting worker slots  */
    U32  cap, head, count;
    int  closed;
    perl_mt_value_t *ring;    /* cap entries, kind 0 when free               */
} mt_chan_t;

static inline void
S_chan_arm_waiter(mt_chan_t *c, uint32_t wslot)
{
    uint32_t word = wslot / PERL_KRT_PBA_BITS;
    uint64_t bit  = (uint64_t)1 << (wslot % PERL_KRT_PBA_BITS);
    atomic_fetch_or_explicit(&c->waiter_bits[word], bit, memory_order_seq_cst);
}

/* A change of the channel: bump seq and wake every waiting worker through
 * its slot vector in S_chan_vec (krt_vec.h).  Dekker seq_cst ordering with
 * S_chan_arm_waiter guarantees no lost wakeup. */
static void
S_chan_changed(mt_chan_t *c)
{
    perl_krt_vectab_t *vt = S_chan_vectab();
    uint32_t w;

    perl_krt_fetch_add_seq_cst(&c->seq, 1);
    if (vt) {
        for (w = 0; w < MT_CHAN_VEC_WORDS; w++) {
            if (perl_krt_load_seq_cst(&c->waiter_bits[w]) != 0) {
                uint64_t mask = atomic_exchange_explicit(&c->waiter_bits[w], 0,
                                                         memory_order_acq_rel);
                while (mask) {
                    uint32_t bit = (uint32_t)__builtin_ctzll(mask);
                    mask &= mask - 1;
                    perl_krt_vec_post(vt, w * PERL_KRT_PBA_BITS + bit);
                }
            }
        }
    }
    else if (perl_krt_load_seq_cst(&c->nwait)) {
        perl_krt_wake_u32((volatile uint32_t *)&c->seq, UINT32_MAX);
    }
}

/* Can a waiter in `mode` go on? */
static bool
S_chan_ready(mt_chan_t *c, int mode)
{
    bool r;
    pthread_mutex_lock(&c->lock);
    r = mode == MT_CHAN_MODE_RECV ? (c->count > 0 || c->closed)
                                  : (c->count < c->cap || c->closed);
    pthread_mutex_unlock(&c->lock);
    return r;
}

/* A scheduled fiber waits on the channel: it parks on PL_mt_chan_waiters, and
 * the scheduler's drain wakes it.  The caller counts itself in c->nwait. */
static void
S_chan_park(pTHX_ UV id, mt_chan_t *c, int mode)
{
    struct mt_fiber * const f = PL_mt_fiber_current;
    f->chan_id = id;
    f->chan_mode = (U32)mode;
    f->chan_flags = 0;
    f->next_chan_waiter = PL_mt_chan_waiters;
    PL_mt_chan_waiters = f;
    Perl_mt_sched_arm(aTHX);
    do {
        Perl_mt_sched_park(aTHX);
    } while (!(f->chan_flags & MT_FIBER_CHAN_DONE));
    PERL_UNUSED_VAR(c);
}

/* Wakes the parked fibers whose channel can go on now.  Called by the poll
 * and the idle hook.  *woke, when not NULL, gets the number woken. */
void
Perl_mt_chan_drain(pTHX_ int *woke)
{
    struct mt_fiber *w, *prev = NULL, *next;
    for (w = PL_mt_chan_waiters; w; w = next) {
        next = w->next_chan_waiter;
        mt_chan_t * const c = (mt_chan_t *)S_obj_get(aTHX_ w->chan_id,
                                                     MT_CHAN_KIND_CHANNEL);
        if (S_chan_ready(c, (int)w->chan_mode)) {
            if (prev)
                prev->next_chan_waiter = next;
            else
                PL_mt_chan_waiters = next;
            w->next_chan_waiter = NULL;
            w->chan_flags |= MT_FIBER_CHAN_DONE;
            Perl_mt_sched_wake(aTHX_ w);
            if (woke)
                (*woke)++;
        }
        else {
            prev = w;
        }
    }
}

/* The idle hook's wait for parked channel waiters: arms this worker's slot
 * vector on every parked waiter's channel, re-checks readiness, and waits on
 * the single slot vector doorbell (immediate cross-worker wake). */
void
Perl_mt_chan_idle_wait(pTHX)
{
    perl_krt_vectab_t *vt;
    struct mt_fiber *w;
    uint32_t wslot, db;

    if (!PL_mt_chan_waiters)
        return;

    vt = S_chan_vectab();
    if (!vt) {
        mt_chan_t *c = (mt_chan_t *)S_obj_get(aTHX_ PL_mt_chan_waiters->chan_id,
                                              MT_CHAN_KIND_CHANNEL);
        uint32_t seen = perl_krt_load_seq_cst(&c->seq);
        if (S_chan_ready(c, (int)PL_mt_chan_waiters->chan_mode))
            return;
        perl_krt_wait_u32((volatile uint32_t *)&c->seq, seen, 1000000ull);
        return;
    }

    wslot = S_chan_worker_slot(aTHX);
    db = perl_krt_vec_doorbell(vt, wslot);

    for (w = PL_mt_chan_waiters; w; w = w->next_chan_waiter) {
        mt_chan_t *c = (mt_chan_t *)S_obj_get(aTHX_ w->chan_id,
                                              MT_CHAN_KIND_CHANNEL);
        S_chan_arm_waiter(c, wslot);
        if (S_chan_ready(c, (int)w->chan_mode))
            return;
    }

    Perl_mt_sup_maybe_beat();
    (void)perl_krt_vec_wait(vt, wslot, db, S_chan_policy(), MT_CHAN_IDLE_WAIT_NS);
}

/* One blocking step of a channel operation: a fiber parks, a thread waits on
 * its own slot vector doorbell. */
static void
S_chan_block(pTHX_ UV id, mt_chan_t *c, int mode, uint32_t wslot,
             uint32_t db, uint32_t seen)
{
    if (MT_FIBER_PARKABLE(PL_mt_fiber_current, PL_mt_sched_fiber)) {
        S_chan_park(aTHX_ id, c, mode);
    }
    else {
        perl_krt_vectab_t *vt = S_chan_vectab();
        Perl_mt_sup_maybe_beat();
        if (vt)
            (void)perl_krt_vec_wait(vt, wslot, db, S_chan_policy(),
                                    MT_CHAN_THREAD_WAIT_NS);
        else
            perl_krt_wait_u32((volatile uint32_t *)&c->seq, seen,
                              MT_CHAN_THREAD_WAIT_NS);
    }
}

/* MT::Channel::_new($capacity) */
XS(XS_MT_Channel__new)
{
    dXSARGS;
    UV cap;
    mt_chan_t *c;
    if (items != 1)
        croak_xs_usage(cv, "capacity");
    cap = SvUV(ST(0));
    if (cap < 1 || cap > 1000000)
        Perl_croak_mt(aTHX_ MT_NO_SUCH_OBJECT);
    Newxz(c, 1, mt_chan_t);
    Newxz(c->ring, cap, perl_mt_value_t);
    c->cap = (U32)cap;
    pthread_mutex_init(&c->lock, NULL);
    ST(0) = sv_2mortal(newSVuv(S_obj_add(MT_CHAN_KIND_CHANNEL, c)));
    XSRETURN(1);
}

/* MT::Channel::_send($id, $value): blocks while the channel is full. */
XS(XS_MT_Channel__send)
{
    dXSARGS;
    perl_mt_value_t v;
    UV id;
    mt_chan_t *c;
    uint32_t wslot;
    perl_krt_vectab_t *vt;
    if (items != 2)
        croak_xs_usage(cv, "id, value");
    id = SvUV(ST(0));
    c = (mt_chan_t *)S_obj_get(aTHX_ id, MT_CHAN_KIND_CHANNEL);
    Perl_mt_value_from_sv(aTHX_ &v, ST(1));          /* may croak: nothing held */
    wslot = S_chan_worker_slot(aTHX);
    vt = S_chan_vectab();
    for (;;) {
        uint32_t seen, db = 0;
        perl_krt_fetch_add_seq_cst(&c->nwait, 1);
        if (vt) {
            db = perl_krt_vec_doorbell(vt, wslot);
            S_chan_arm_waiter(c, wslot);
        }
        pthread_mutex_lock(&c->lock);
        if (c->closed) {
            pthread_mutex_unlock(&c->lock);
            perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
            Perl_mt_value_free(&v);
            Perl_croak_mt(aTHX_ MT_CHAN_CLOSED);
        }
        if (c->count < c->cap) {
            c->ring[(c->head + c->count) % c->cap] = v;
            c->count++;
            pthread_mutex_unlock(&c->lock);
            perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
            S_chan_changed(c);
            XSRETURN_YES;
        }
        seen = perl_krt_load_seq_cst(&c->seq);
        pthread_mutex_unlock(&c->lock);
        S_chan_block(aTHX_ id, c, MT_CHAN_MODE_SEND, wslot, db, seen);
        perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
    }
}

/* MT::Channel::_recv($id): the next value, or undef once the channel is
 * closed and empty.  Blocks while it is empty. */
XS(XS_MT_Channel__recv)
{
    dXSARGS;
    UV id;
    mt_chan_t *c;
    uint32_t wslot;
    perl_krt_vectab_t *vt;
    if (items != 1)
        croak_xs_usage(cv, "id");
    id = SvUV(ST(0));
    c = (mt_chan_t *)S_obj_get(aTHX_ id, MT_CHAN_KIND_CHANNEL);
    wslot = S_chan_worker_slot(aTHX);
    vt = S_chan_vectab();
    for (;;) {
        uint32_t seen, db = 0;
        perl_krt_fetch_add_seq_cst(&c->nwait, 1);
        if (vt) {
            db = perl_krt_vec_doorbell(vt, wslot);
            S_chan_arm_waiter(c, wslot);
        }
        pthread_mutex_lock(&c->lock);
        if (c->count > 0) {
            perl_mt_value_t v = c->ring[c->head];
            SV *sv;
            Zero(&c->ring[c->head], 1, perl_mt_value_t);
            c->head = (c->head + 1) % c->cap;
            c->count--;
            pthread_mutex_unlock(&c->lock);
            perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
            S_chan_changed(c);
            sv = Perl_mt_value_to_sv(aTHX_ &v);
            Perl_mt_value_free(&v);
            ST(0) = sv_2mortal(sv);
            XSRETURN(1);
        }
        if (c->closed) {
            pthread_mutex_unlock(&c->lock);
            perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
            XSRETURN_UNDEF;
        }
        seen = perl_krt_load_seq_cst(&c->seq);
        pthread_mutex_unlock(&c->lock);
        S_chan_block(aTHX_ id, c, MT_CHAN_MODE_RECV, wslot, db, seen);
        perl_krt_fetch_add_seq_cst(&c->nwait, (uint32_t)-1);
    }
}

/* MT::Channel::_try_recv($id): (1, $value), or (0) when nothing is there. */
XS(XS_MT_Channel__try_recv)
{
    dXSARGS;
    UV id;
    mt_chan_t *c;
    if (items != 1)
        croak_xs_usage(cv, "id");
    id = SvUV(ST(0));
    c = (mt_chan_t *)S_obj_get(aTHX_ id, MT_CHAN_KIND_CHANNEL);
    pthread_mutex_lock(&c->lock);
    if (c->count == 0) {
        pthread_mutex_unlock(&c->lock);
        XSRETURN_NO;
    }
    {
        perl_mt_value_t v = c->ring[c->head];
        SV *sv;
        Zero(&c->ring[c->head], 1, perl_mt_value_t);
        c->head = (c->head + 1) % c->cap;
        c->count--;
        pthread_mutex_unlock(&c->lock);
        S_chan_changed(c);
        sv = Perl_mt_value_to_sv(aTHX_ &v);
        Perl_mt_value_free(&v);
        ST(0) = sv_2mortal(newSViv(1));
        ST(1) = sv_2mortal(sv);
        XSRETURN(2);
    }
}

/* MT::Channel::_close($id): no more sends; receivers drain what is queued. */
XS(XS_MT_Channel__close)
{
    dXSARGS;
    mt_chan_t *c;
    if (items != 1)
        croak_xs_usage(cv, "id");
    c = (mt_chan_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_CHANNEL);
    pthread_mutex_lock(&c->lock);
    c->closed = 1;
    pthread_mutex_unlock(&c->lock);
    S_chan_changed(c);
    XSRETURN_EMPTY;
}

/* MT::Channel::_len($id): the number of values queued. */
XS(XS_MT_Channel__len)
{
    dXSARGS;
    mt_chan_t *c;
    U32 n;
    if (items != 1)
        croak_xs_usage(cv, "id");
    c = (mt_chan_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_CHANNEL);
    pthread_mutex_lock(&c->lock);
    n = c->count;
    pthread_mutex_unlock(&c->lock);
    ST(0) = sv_2mortal(newSVuv(n));
    XSRETURN(1);
}

/* ---- cells (lock-free body-pointer CAS with hazard pointers) ------------ */

#define MT_CELL_MAX_HP           256u
#define MT_CELL_RETIRE_THRESHOLD 32u

typedef struct mt_cell_body {
    perl_mt_value_t      v;
    struct mt_cell_body *retire_next;
} mt_cell_body_t;

typedef struct mt_cell_hp {
    PERL_KRT_CACHE_ALIGNED
    perl_krt_aptr_t  ptr;         /* currently protected mt_cell_body_t * */
    perl_krt_au32_t  in_use;      /* 1 if claimed by a live thread        */
    mt_cell_body_t  *retired;     /* thread-local retired list            */
    uint32_t         nretired;
} mt_cell_hp_t;

typedef struct mt_cell {
    perl_krt_aptr_t body;         /* mt_cell_body_t *, atomic CAS target  */
} mt_cell_t;

static mt_cell_hp_t    S_cell_hps[MT_CELL_MAX_HP];
static pthread_once_t  S_cell_hp_once = PTHREAD_ONCE_INIT;
static pthread_key_t   S_cell_hp_key;
static pthread_mutex_t S_cell_orphan_lock = PTHREAD_MUTEX_INITIALIZER;
static mt_cell_body_t *S_cell_orphans = NULL;

static void
S_cell_body_free(mt_cell_body_t *b)
{
    if (!b)
        return;
    Perl_mt_value_free(&b->v);
    PerlMemShared_free(b);
}

static int
S_cell_is_hazardous(mt_cell_body_t *b)
{
    uint32_t i;
    for (i = 0; i < MT_CELL_MAX_HP; i++) {
        if (atomic_load_explicit(&S_cell_hps[i].in_use, memory_order_acquire)
            && atomic_load_explicit(&S_cell_hps[i].ptr, memory_order_seq_cst) == b)
            return 1;
    }
    return 0;
}

static void
S_cell_hp_scan(mt_cell_hp_t *hp)
{
    mt_cell_body_t *list = hp->retired;
    mt_cell_body_t *keep = NULL;
    uint32_t nkeep = 0;

    hp->retired = NULL;
    hp->nretired = 0;

    while (list) {
        mt_cell_body_t *next = list->retire_next;
        if (S_cell_is_hazardous(list)) {
            list->retire_next = keep;
            keep = list;
            nkeep++;
        }
        else {
            S_cell_body_free(list);
        }
        list = next;
    }
    hp->retired = keep;
    hp->nretired = nkeep;
}

static void
S_cell_hp_thread_exit(void *arg)
{
    mt_cell_hp_t *hp = (mt_cell_hp_t *)arg;
    if (!hp)
        return;
    atomic_store_explicit(&hp->ptr, NULL, memory_order_seq_cst);
    S_cell_hp_scan(hp);
    if (hp->retired) {
        mt_cell_body_t *tail = hp->retired;
        while (tail->retire_next)
            tail = tail->retire_next;
        pthread_mutex_lock(&S_cell_orphan_lock);
        tail->retire_next = S_cell_orphans;
        S_cell_orphans = hp->retired;
        pthread_mutex_unlock(&S_cell_orphan_lock);
        hp->retired = NULL;
        hp->nretired = 0;
    }
    atomic_store_explicit(&hp->in_use, 0u, memory_order_release);
}

static void
S_cell_hp_init_once(void)
{
    pthread_key_create(&S_cell_hp_key, S_cell_hp_thread_exit);
}

static mt_cell_hp_t *
S_cell_hp_acquire(void)
{
    mt_cell_hp_t *hp;
    uint32_t i;

    pthread_once(&S_cell_hp_once, S_cell_hp_init_once);
    hp = (mt_cell_hp_t *)pthread_getspecific(S_cell_hp_key);
    if (LIKELY(hp != NULL))
        return hp;

    for (i = 0; i < MT_CELL_MAX_HP; i++) {
        uint32_t expected = 0u;
        if (atomic_compare_exchange_strong_explicit(&S_cell_hps[i].in_use,
                                                    &expected, 1u,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            hp = &S_cell_hps[i];
            atomic_store_explicit(&hp->ptr, NULL, memory_order_relaxed);
            hp->retired = NULL;
            hp->nretired = 0;
            pthread_setspecific(S_cell_hp_key, hp);
            return hp;
        }
    }
    return NULL;
}

static inline mt_cell_body_t *
S_cell_hp_protect(mt_cell_t *c, mt_cell_hp_t *hp)
{
    mt_cell_body_t *p;
    do {
        p = (mt_cell_body_t *)atomic_load_explicit(&c->body,
                                                   memory_order_acquire);
        atomic_store_explicit(&hp->ptr, p, memory_order_seq_cst);
    } while (p != (mt_cell_body_t *)atomic_load_explicit(&c->body,
                                                         memory_order_seq_cst));
    return p;
}

static inline void
S_cell_hp_clear(mt_cell_hp_t *hp)
{
    atomic_store_explicit(&hp->ptr, NULL, memory_order_release);
}

static void
S_cell_hp_retire(mt_cell_hp_t *hp, mt_cell_body_t *old)
{
    if (!old)
        return;
    old->retire_next = hp->retired;
    hp->retired = old;
    hp->nretired++;
    if (hp->nretired >= MT_CELL_RETIRE_THRESHOLD)
        S_cell_hp_scan(hp);
}

/* Two stored values are the same: the same kind and contents. */
static bool
S_val_eq(const perl_mt_value_t *a, const perl_mt_value_t *b)
{
    if (a->kind != b->kind)
        return FALSE;
    switch (a->kind) {
    case 0:
    case 'u': return TRUE;
    case 'i': return a->iv == b->iv;
    case 'n': return a->nv == b->nv;
    case 'p': return a->len == b->len && a->utf8 == b->utf8
                  && memcmp(a->pv, b->pv, (size_t)a->len) == 0;
    case 'c': return a->iv == b->iv && a->gen == b->gen;
    default:  return FALSE;
    }
}

/* MT::Cell::_new($value) */
XS(XS_MT_Cell__new)
{
    dXSARGS;
    perl_mt_value_t v;
    mt_cell_body_t *b;
    mt_cell_t *c;
    if (items != 1)
        croak_xs_usage(cv, "value");
    Perl_mt_value_from_sv(aTHX_ &v, ST(0));  /* may croak before alloc */
    b = (mt_cell_body_t *)PerlMemShared_calloc(1, sizeof *b);
    if (!b) {
        Perl_mt_value_free(&v);
        croak_no_mem();
    }
    b->v = v;
    Newxz(c, 1, mt_cell_t);
    atomic_init(&c->body, b);
    ST(0) = sv_2mortal(newSVuv(S_obj_add(MT_CHAN_KIND_CELL, c)));
    XSRETURN(1);
}

/* MT::Cell::_get($id): lock-free read via hazard pointer */
XS(XS_MT_Cell__get)
{
    dXSARGS;
    mt_cell_t *c;
    mt_cell_hp_t *hp;
    mt_cell_body_t *b;
    SV *sv;
    if (items != 1)
        croak_xs_usage(cv, "id");
    c = (mt_cell_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_CELL);
    hp = S_cell_hp_acquire();
    b = S_cell_hp_protect(c, hp);
    sv = Perl_mt_value_to_sv(aTHX_ &b->v);
    S_cell_hp_clear(hp);
    ST(0) = sv_2mortal(sv);
    XSRETURN(1);
}

/* MT::Cell::_set($id, $value): lock-free atomic pointer swap */
XS(XS_MT_Cell__set)
{
    dXSARGS;
    mt_cell_t *c;
    mt_cell_hp_t *hp;
    perl_mt_value_t v;
    mt_cell_body_t *nb, *old;
    if (items != 2)
        croak_xs_usage(cv, "id, value");
    c = (mt_cell_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_CELL);
    Perl_mt_value_from_sv(aTHX_ &v, ST(1));  /* may croak before alloc */
    nb = (mt_cell_body_t *)PerlMemShared_calloc(1, sizeof *nb);
    if (!nb) {
        Perl_mt_value_free(&v);
        croak_no_mem();
    }
    nb->v = v;
    hp = S_cell_hp_acquire();
    old = (mt_cell_body_t *)atomic_exchange_explicit(&c->body, nb,
                                                     memory_order_acq_rel);
    S_cell_hp_retire(hp, old);
    XSRETURN_EMPTY;
}

/* MT::Cell::_cas($id, $expected, $new): lock-free CAS on the body pointer.
 * Returns 1 when the stored value equalled $expected and was atomically
 * replaced with $new, 0 otherwise. */
XS(XS_MT_Cell__cas)
{
    dXSARGS;
    mt_cell_t *c;
    mt_cell_hp_t *hp;
    perl_mt_value_t e, n;
    mt_cell_body_t *nb;
    if (items != 3)
        croak_xs_usage(cv, "id, expected, new");
    c = (mt_cell_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_CELL);
    Perl_mt_value_from_sv(aTHX_ &e, ST(1));
    Perl_mt_value_from_sv(aTHX_ &n, ST(2));
    nb = (mt_cell_body_t *)PerlMemShared_calloc(1, sizeof *nb);
    if (!nb) {
        Perl_mt_value_free(&e);
        Perl_mt_value_free(&n);
        croak_no_mem();
    }
    nb->v = n;
    hp = S_cell_hp_acquire();
    for (;;) {
        mt_cell_body_t *cur = S_cell_hp_protect(c, hp);
        void *exp_ptr;
        if (!S_val_eq(&cur->v, &e)) {
            S_cell_hp_clear(hp);
            Perl_mt_value_free(&e);
            S_cell_body_free(nb);
            XSRETURN_NO;
        }
        exp_ptr = cur;
        if (atomic_compare_exchange_strong_explicit(&c->body, &exp_ptr, nb,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            S_cell_hp_clear(hp);
            Perl_mt_value_free(&e);
            S_cell_hp_retire(hp, cur);
            XSRETURN_YES;
        }
    }
}

/* ---- shared hashes ---------------------------------------------------------------- */

typedef struct mt_sh_node {
    struct mt_sh_node *next;
    char   *key;
    STRLEN  klen;
    bool    utf8;
    perl_mt_value_t v;
} mt_sh_node_t;

typedef struct mt_sh_shard {
    pthread_mutex_t lock;
    mt_sh_node_t   *bucket[MT_SH_BUCKETS];
} mt_sh_shard_t;

typedef struct mt_shared {
    mt_sh_shard_t shard[MT_SH_SHARDS];
} mt_shared_t;

/* FNV-1a over the key's bytes */
static U32
S_sh_hash(const char *k, STRLEN len)
{
    U32 h = 2166136261u;
    STRLEN i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)k[i];
        h *= 16777619u;
    }
    return h;
}

/* The node of `key` in its shard, or NULL.  The shard's lock is held. */
static mt_sh_node_t *
S_sh_find(mt_shared_t *h, U32 hash, const char *k, STRLEN klen, bool utf8,
          mt_sh_shard_t **shp, mt_sh_node_t ***linkp)
{
    mt_sh_shard_t * const sh = &h->shard[hash % MT_SH_SHARDS];
    mt_sh_node_t **link = &sh->bucket[(hash / MT_SH_SHARDS) % MT_SH_BUCKETS];
    mt_sh_node_t *n;
    *shp = sh;
    for (n = *link; n; link = &n->next, n = n->next) {
        if (n->klen == klen && n->utf8 == utf8 && memcmp(n->key, k, klen) == 0) {
            *linkp = link;
            return n;
        }
    }
    *linkp = link;
    return NULL;
}

/* MT::SharedHash::_new() */
XS(XS_MT_SharedHash__new)
{
    dXSARGS;
    mt_shared_t *h;
    U32 i;
    PERL_UNUSED_VAR(items);
    Newxz(h, 1, mt_shared_t);
    for (i = 0; i < MT_SH_SHARDS; i++)
        pthread_mutex_init(&h->shard[i].lock, NULL);
    ST(0) = sv_2mortal(newSVuv(S_obj_add(MT_CHAN_KIND_SHARED, h)));
    XSRETURN(1);
}

/* MT::SharedHash::_get($id, $key): the value, or undef */
XS(XS_MT_SharedHash__get)
{
    dXSARGS;
    mt_shared_t *h;
    STRLEN klen;
    const char *k;
    U32 hash;
    mt_sh_shard_t *sh;
    mt_sh_node_t **link, *n;
    SV *sv = NULL;
    if (items != 2)
        croak_xs_usage(cv, "id, key");
    h = (mt_shared_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_SHARED);
    k = SvPV_const(ST(1), klen);
    hash = S_sh_hash(k, klen);
    n = S_sh_find(h, hash, k, klen, cBOOL(SvUTF8(ST(1))), &sh, &link);
    pthread_mutex_lock(&sh->lock);
    n = S_sh_find(h, hash, k, klen, cBOOL(SvUTF8(ST(1))), &sh, &link);
    if (n)
        sv = Perl_mt_value_to_sv(aTHX_ &n->v);
    pthread_mutex_unlock(&sh->lock);
    ST(0) = sv ? sv_2mortal(sv) : &PL_sv_undef;
    XSRETURN(1);
}

/* MT::SharedHash::_set($id, $key, $value) */
XS(XS_MT_SharedHash__set)
{
    dXSARGS;
    mt_shared_t *h;
    STRLEN klen;
    const char *k;
    U32 hash;
    bool utf8;
    perl_mt_value_t v;
    mt_sh_shard_t *sh;
    mt_sh_node_t **link, *n;
    if (items != 3)
        croak_xs_usage(cv, "id, key, value");
    h = (mt_shared_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_SHARED);
    k = SvPV_const(ST(1), klen);
    utf8 = cBOOL(SvUTF8(ST(1)));
    hash = S_sh_hash(k, klen);
    Perl_mt_value_from_sv(aTHX_ &v, ST(2));
    S_sh_find(h, hash, k, klen, utf8, &sh, &link);   /* picks the shard */
    pthread_mutex_lock(&sh->lock);
    n = S_sh_find(h, hash, k, klen, utf8, &sh, &link);
    if (n) {
        perl_mt_value_t old = n->v;
        n->v = v;
        pthread_mutex_unlock(&sh->lock);
        Perl_mt_value_free(&old);
    }
    else {
        Newx(n, 1, mt_sh_node_t);
        Newx(n->key, klen + 1, char);
        Copy(k, n->key, klen, char);
        n->key[klen] = '\0';
        n->klen = klen;
        n->utf8 = utf8;
        n->v = v;
        n->next = NULL;
        *link = n;
        pthread_mutex_unlock(&sh->lock);
    }
    XSRETURN_EMPTY;
}

/* MT::SharedHash::_delete($id, $key): 1 when it was there */
XS(XS_MT_SharedHash__delete)
{
    dXSARGS;
    mt_shared_t *h;
    STRLEN klen;
    const char *k;
    U32 hash;
    bool utf8;
    mt_sh_shard_t *sh;
    mt_sh_node_t **link, *n;
    if (items != 2)
        croak_xs_usage(cv, "id, key");
    h = (mt_shared_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_SHARED);
    k = SvPV_const(ST(1), klen);
    utf8 = cBOOL(SvUTF8(ST(1)));
    hash = S_sh_hash(k, klen);
    S_sh_find(h, hash, k, klen, utf8, &sh, &link);   /* picks the shard */
    pthread_mutex_lock(&sh->lock);
    n = S_sh_find(h, hash, k, klen, utf8, &sh, &link);
    if (n) {
        *link = n->next;
        pthread_mutex_unlock(&sh->lock);
        Perl_mt_value_free(&n->v);
        Safefree(n->key);
        Safefree(n);
        XSRETURN_YES;
    }
    pthread_mutex_unlock(&sh->lock);
    XSRETURN_NO;
}

/* MT::SharedHash::_exists($id, $key): 1 when the key is there */
XS(XS_MT_SharedHash__exists)
{
    dXSARGS;
    mt_shared_t *h;
    STRLEN klen;
    const char *k;
    U32 hash;
    bool utf8, found;
    mt_sh_shard_t *sh;
    mt_sh_node_t **link;
    if (items != 2)
        croak_xs_usage(cv, "id, key");
    h = (mt_shared_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_SHARED);
    k = SvPV_const(ST(1), klen);
    utf8 = cBOOL(SvUTF8(ST(1)));
    hash = S_sh_hash(k, klen);
    S_sh_find(h, hash, k, klen, utf8, &sh, &link);   /* picks the shard */
    pthread_mutex_lock(&sh->lock);
    found = S_sh_find(h, hash, k, klen, utf8, &sh, &link) != NULL;
    pthread_mutex_unlock(&sh->lock);
    if (found)
        XSRETURN_YES;
    XSRETURN_NO;
}

/* MT::SharedHash::_keys($id): a snapshot of the keys, shard by shard */
XS(XS_MT_SharedHash__keys)
{
    dXSARGS;
    mt_shared_t *h;
    U32 i, b;
    if (items != 1)
        croak_xs_usage(cv, "id");
    h = (mt_shared_t *)S_obj_get(aTHX_ SvUV(ST(0)), MT_CHAN_KIND_SHARED);
    SP -= items;
    for (i = 0; i < MT_SH_SHARDS; i++) {
        pthread_mutex_lock(&h->shard[i].lock);
        for (b = 0; b < MT_SH_BUCKETS; b++) {
            mt_sh_node_t *n;
            for (n = h->shard[i].bucket[b]; n; n = n->next) {
                SV * const ks = newSVpvn(n->key, n->klen);
                if (n->utf8)
                    SvUTF8_on(ks);
                XPUSHs(sv_2mortal(ks));
            }
        }
        pthread_mutex_unlock(&h->shard[i].lock);
    }
    PUTBACK;
    return;
}

/* ---- registration ------------------------------------------------------------------ */

void
Perl_mt_boot_chan(pTHX)
{
    newXS_flags("MT::Channel::_new",     XS_MT_Channel__new,     __FILE__, "$", 0);
    newXS_flags("MT::Channel::_send",    XS_MT_Channel__send,    __FILE__, "$$", 0);
    newXS_flags("MT::Channel::_recv",    XS_MT_Channel__recv,    __FILE__, "$", 0);
    newXS_flags("MT::Channel::_try_recv", XS_MT_Channel__try_recv, __FILE__, "$", 0);
    newXS_flags("MT::Channel::_close",   XS_MT_Channel__close,   __FILE__, "$", 0);
    newXS_flags("MT::Channel::_len",     XS_MT_Channel__len,     __FILE__, "$", 0);
    newXS_flags("MT::Cell::_new",        XS_MT_Cell__new,        __FILE__, "$", 0);
    newXS_flags("MT::Cell::_get",        XS_MT_Cell__get,        __FILE__, "$", 0);
    newXS_flags("MT::Cell::_set",        XS_MT_Cell__set,        __FILE__, "$$", 0);
    newXS_flags("MT::Cell::_cas",        XS_MT_Cell__cas,        __FILE__, "$$$", 0);
    newXS_flags("MT::SharedHash::_new",  XS_MT_SharedHash__new,  __FILE__, "", 0);
    newXS_flags("MT::SharedHash::_get", XS_MT_SharedHash__get, __FILE__, "$$", 0);
    newXS_flags("MT::SharedHash::_set", XS_MT_SharedHash__set, __FILE__, "$$$", 0);
    newXS_flags("MT::SharedHash::_delete", XS_MT_SharedHash__delete, __FILE__, "$$", 0);
    newXS_flags("MT::SharedHash::_exists", XS_MT_SharedHash__exists, __FILE__, "$$", 0);
    newXS_flags("MT::SharedHash::_keys", XS_MT_SharedHash__keys, __FILE__, "$", 0);
}

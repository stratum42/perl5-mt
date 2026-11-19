/*    krt_vec.h
 *
 *    The vector table and the doorbell wait.  Header-only and
 *    position independent, like krt_ring.h: the table is laid out in
 *    caller memory (a perl_krt_shm_t segment mapped at different
 *    addresses in different processes), and every reference inside it is
 *    an offset from the table header.
 *
 *    The model is MSI-X in miniature:
 *
 *      vector      one per source (a request, a reply, an XS call).  Its
 *                  doorbell is bumped by every post and is the word the
 *                  waiter sleeps on; state, mask, cpu and owner are the
 *                  waiter's side of the handshake.
 *      pending     one bit per vector, in a pending-bit array of 64 bits
 *                  per word.  A post sets the bit, so posts that arrive
 *                  together merge into one pending event, which the
 *                  server takes in one exchange.
 *      word        one doorbell per 64 vectors: a server that serves many
 *                  vectors waits on one word, not on each vector.
 *      mask        a masked vector keeps its pending bit and does not
 *                  wake its waiter; unmasking wakes a parked waiter.
 *
 *    The waiter polls the doorbell for a short budget (perl_krt_wait_line)
 *    and then parks in perl_krt_wait_u32_flags.  The poster bumps the
 *    doorbell and then reads the waiter's state.  Both sides use seq_cst
 *    for the store / load pair, so at least one of them sees the other
 *    (the same Dekker pairing krt_ring.h relies on): either the poster sees
 *    PARKED and wakes the waiter, or the waiter's re-read of the doorbell
 *    sees the bump and the waiter does not sleep.  The futex / ulock
 *    itself compares the doorbell with `expected` atomically, so a post
 *    that lands between the re-read and the sleep also fails the sleep.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_VEC_H
#define PERL_KRT_VEC_H

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "krt.h"
#include "krt_atomic.h"
#include "krt_wait.h"

typedef _Atomic int32_t perl_krt_ai32_t;

#define PERL_KRT_VEC_MAGIC     0x5452564bu   /* 'KRVT' little-endian */
#define PERL_KRT_VEC_VERSION   1u

/* state of a waiter, as seen by its posters                              */
#define PERL_KRT_VEC_RUNNING   0u
#define PERL_KRT_VEC_POLLING   1u
#define PERL_KRT_VEC_PARKED    2u

#define PERL_KRT_PBA_BITS      64u

/* Defaults for the adaptive policy (2 us to 50 us).                      */
#define PERL_KRT_POLL_DEFAULT_MIN_NS  2000ull
#define PERL_KRT_POLL_DEFAULT_MAX_NS  50000ull

/* ---- the adaptive poll policy ------------------------------------------ */

typedef struct perl_krt_poll_policy {
    uint64_t min_ns;      /* floor of cur_ns (1 us when 0 is asked for)   */
    uint64_t max_ns;      /* ceiling of cur_ns                             */
    uint64_t cur_ns;      /* spin budget of the next wait; 0 = park at once */
    uint32_t hits;        /* waits that ended CHANGED while polling        */
    uint32_t misses;      /* waits that had to park                        */
} perl_krt_poll_policy_t;

/* PERL_KRT_POLL_NS in the environment pins the budget at that many
 * nanoseconds (min = max = cur, so no adaptation); 0 disables polling.    */
static inline void
perl_krt_poll_policy_init(perl_krt_poll_policy_t *p, uint64_t min_ns,
                          uint64_t max_ns)
{
    const char *e = getenv("PERL_KRT_POLL_NS");
    char *end = NULL;
    unsigned long long v;
    p->hits = 0;
    p->misses = 0;
    if (e && *e) {
        errno = 0;
        v = strtoull(e, &end, 10);
        if (errno == 0 && end != e && *end == '\0') {
            p->min_ns = p->max_ns = p->cur_ns = (uint64_t)v;
            return;
        }
    }
    if (min_ns > max_ns)
        min_ns = max_ns;
    if (min_ns == 0 && max_ns != 0)
        min_ns = 1000;
    p->min_ns = min_ns;
    p->max_ns = max_ns;
    p->cur_ns = min_ns;
}

/* A poll that saw the change: the next poll is worth more.  NAPI-style:
 * doubles toward max.  A wait that had to park halves toward min.        */
static inline void
perl_krt_poll_policy_hit(perl_krt_poll_policy_t *p)
{
    p->hits++;
    if (p->min_ns == p->max_ns)
        return;                               /* pinned */
    p->cur_ns = p->cur_ns * 2;
    if (p->cur_ns > p->max_ns)
        p->cur_ns = p->max_ns;
}

static inline void
perl_krt_poll_policy_miss(perl_krt_poll_policy_t *p)
{
    p->misses++;
    if (p->min_ns == p->max_ns)
        return;                               /* pinned */
    p->cur_ns = p->cur_ns / 2;
    if (p->cur_ns < p->min_ns)
        p->cur_ns = p->min_ns;
}

/* Learn the budget from a wait that ended CHANGED after `elapsed` ns.
 * A budget that is always shorter than the round trip never hits, and
 * the halving rule above can only shrink it further, so the policy must
 * learn from the durations it sees.  A wait within max is worth polling
 * for at its own length plus a quarter, smoothed; a wait beyond max is
 * not worth any spin, so the budget goes back to the floor.  (POWER8: the
 * floor of 2 us never hit a 10 us round trip, and every wait parked.)     */
static inline void
perl_krt_poll_policy_learn(perl_krt_poll_policy_t *p, uint64_t elapsed_ns)
{
    uint64_t target;
    if (p->min_ns == p->max_ns)
        return;                               /* pinned */
    if (elapsed_ns > p->max_ns) {
        p->cur_ns = p->min_ns;
        return;
    }
    target = elapsed_ns + elapsed_ns / 4;
    if (target < p->min_ns)
        target = p->min_ns;
    if (target > p->max_ns)
        target = p->max_ns;
    p->cur_ns = (p->cur_ns + target) / 2;
}

/* ---- the table ---------------------------------------------------------- */

typedef struct perl_krt_vec {
    perl_krt_au32_t  doorbell;   /* bumped by every post; the waiter's word */
    perl_krt_au32_t  state;      /* PERL_KRT_VEC_RUNNING / POLLING / PARKED */
    perl_krt_au32_t  mask;       /* nonzero: post leaves the bit pending    */
    perl_krt_ai32_t  cpu;        /* waiter's last CPU (perl_krt_cpu_id), -1 */
    uint32_t         owner_pid;  /* pid of the waiter, set by claim         */
    uint32_t         gen;        /* bumped by every claim                   */
} __attribute__((aligned(PERL_KRT_CACHE_LINE))) perl_krt_vec_t;

typedef struct perl_krt_pba_word {
    perl_krt_au64_t  bits;       /* pending bits for 64 vectors             */
    perl_krt_au32_t  doorbell;   /* bumped whenever a bit is set            */
    perl_krt_au32_t  state;      /* the word's waiter, as for a vector      */
} __attribute__((aligned(PERL_KRT_CACHE_LINE))) perl_krt_pba_word_t;

typedef struct perl_krt_vectab {
    perl_krt_au32_t  magic;      /* stored last, release; attach checks it  */
    uint32_t         version;
    uint32_t         nvec;
    uint32_t         npba;
    uint32_t         vec_off;    /* bytes from the header to vector 0       */
    uint32_t         pba_off;    /* bytes from the header to word 0         */
    uint64_t         total;      /* bytes used                              */
} __attribute__((aligned(PERL_KRT_CACHE_LINE))) perl_krt_vectab_t;

_Static_assert(sizeof(perl_krt_vec_t) == PERL_KRT_CACHE_LINE,
               "a vector is one cache line");
_Static_assert(sizeof(perl_krt_pba_word_t) == PERL_KRT_CACHE_LINE,
               "a pending-bit word is one cache line");

static inline size_t
perl_krt_vectab_size(uint32_t nvec)
{
    uint32_t npba;
    if (nvec == 0 || nvec > (1u << 20))
        return 0;
    npba = (nvec + PERL_KRT_PBA_BITS - 1) / PERL_KRT_PBA_BITS;
    return sizeof(perl_krt_vectab_t)
         + (size_t)nvec * sizeof(perl_krt_vec_t)
         + (size_t)npba * sizeof(perl_krt_pba_word_t);
}

static inline perl_krt_vec_t *
perl_krt_vec_at(perl_krt_vectab_t *t, uint32_t idx)
{
    return (perl_krt_vec_t *)((char *)t + t->vec_off) + idx;
}

static inline perl_krt_pba_word_t *
perl_krt_pba_at(perl_krt_vectab_t *t, uint32_t word)
{
    return (perl_krt_pba_word_t *)((char *)t + t->pba_off) + word;
}

static inline perl_krt_vectab_t *
perl_krt_vectab_init(void *mem, size_t len, uint32_t nvec)
{
    perl_krt_vectab_t *t = (perl_krt_vectab_t *)mem;
    size_t need = perl_krt_vectab_size(nvec);
    uint32_t i;
    if (!mem || need == 0 || len < need)
        return NULL;
    memset(t, 0, need);
    t->version = PERL_KRT_VEC_VERSION;
    t->nvec    = nvec;
    t->npba    = (nvec + PERL_KRT_PBA_BITS - 1) / PERL_KRT_PBA_BITS;
    t->vec_off = (uint32_t)sizeof(perl_krt_vectab_t);
    t->pba_off = t->vec_off + nvec * (uint32_t)sizeof(perl_krt_vec_t);
    t->total   = need;
    for (i = 0; i < nvec; i++) {
        perl_krt_vec_t *v = perl_krt_vec_at(t, i);
        atomic_init(&v->doorbell, 0);
        atomic_init(&v->state, PERL_KRT_VEC_RUNNING);
        atomic_init(&v->mask, 0);
        atomic_init(&v->cpu, -1);
        v->owner_pid = 0;
        v->gen = 0;
    }
    for (i = 0; i < t->npba; i++) {
        perl_krt_pba_word_t *w = perl_krt_pba_at(t, i);
        atomic_init(&w->bits, 0);
        atomic_init(&w->doorbell, 0);
        atomic_init(&w->state, PERL_KRT_VEC_RUNNING);
    }
    atomic_init(&t->magic, 0);
    perl_krt_store_release(&t->magic, PERL_KRT_VEC_MAGIC);
    return t;
}

/* Attach to a table another process initialised in shared memory.  NULL
 * when the header is not ours, or does not fit in len.                    */
static inline perl_krt_vectab_t *
perl_krt_vectab_attach(void *mem, size_t len)
{
    perl_krt_vectab_t *t = (perl_krt_vectab_t *)mem;
    if (!mem || len < sizeof *t)
        return NULL;
    if (perl_krt_load_acquire(&t->magic) != PERL_KRT_VEC_MAGIC)
        return NULL;
    if (t->version != PERL_KRT_VEC_VERSION || t->nvec == 0)
        return NULL;
    if (t->total != perl_krt_vectab_size(t->nvec) || t->total > len)
        return NULL;
    return t;
}

/* ---- wake helpers ------------------------------------------------------- */

static inline void
perl_krt_vec_wake(perl_krt_au32_t *doorbell)
{
    perl_krt_wake_u32_flags((volatile uint32_t *)doorbell, UINT32_MAX,
                            PERL_KRT_WAIT_SHARED);
}

/* The vector's doorbell, read before the caller looks for work (see
 * perl_krt_vec_wait).                                                    */
static inline uint32_t
perl_krt_vec_doorbell(perl_krt_vectab_t *t, uint32_t idx)
{
    return perl_krt_load_acquire(&perl_krt_vec_at(t, idx)->doorbell);
}

/* ---- post --------------------------------------------------------------- */

/* Set the pending bit of vector idx and ring its doorbells.  Returns 1 if
 * a wake system call was made, 0 otherwise (the waiter was polling, or
 * the vector is masked).                                                  */
static inline int
perl_krt_vec_post(perl_krt_vectab_t *t, uint32_t idx)
{
    perl_krt_vec_t *v = perl_krt_vec_at(t, idx);
    perl_krt_pba_word_t *w = perl_krt_pba_at(t, idx / PERL_KRT_PBA_BITS);
    uint64_t bit = (uint64_t)1 << (idx % PERL_KRT_PBA_BITS);
    int woke = 0;

    /* the bit is data: release is enough, the doorbell bump orders it  */
    atomic_fetch_or_explicit(&w->bits, bit, memory_order_release);
    /* the doorbells are the Dekker store: seq_cst, so the state loads
     * below cannot be reordered before them                             */
    perl_krt_fetch_add_seq_cst(&w->doorbell, 1);
    perl_krt_fetch_add_seq_cst(&v->doorbell, 1);

    if (perl_krt_load_seq_cst(&v->mask) == 0) {
        if (perl_krt_load_seq_cst(&v->state) == PERL_KRT_VEC_PARKED) {
            perl_krt_vec_wake(&v->doorbell);
            woke = 1;
        }
        if (perl_krt_load_seq_cst(&w->state) == PERL_KRT_VEC_PARKED) {
            perl_krt_vec_wake(&w->doorbell);
            woke = 1;
        }
    }
    return woke;
}

/* ---- the doorbell wait -------------------------------------------------- */

/* Wait until the doorbell differs from expected, or timeout_ns passes.
 * Polls for policy->cur_ns first (no system call), then parks.  NULL
 * policy parks at once.  Returns PERL_KRT_WAIT_CHANGED or _TIMEOUT.      */
static inline perl_krt_wait_reason_t
perl_krt_db_wait_raw(perl_krt_au32_t *doorbell, perl_krt_au32_t *state,
                     uint32_t expected, perl_krt_poll_policy_t *p,
                     uint64_t timeout_ns)
{
    uint64_t deadline = PERL_KRT_WAIT_FOREVER;
    uint64_t budget;
    perl_krt_wait_reason_t why;

    if (timeout_ns != PERL_KRT_WAIT_FOREVER && timeout_ns < (1ull << 62))
        deadline = perl_krt_now_ns() + timeout_ns;

    if (p && p->cur_ns) {
        budget = p->cur_ns;
        if (deadline != PERL_KRT_WAIT_FOREVER) {
            uint64_t now = perl_krt_now_ns();
            uint64_t left = now < deadline ? deadline - now : 0;
            if (budget > left)
                budget = left;
        }
        perl_krt_store_seq_cst(state, PERL_KRT_VEC_POLLING);
        why = perl_krt_wait_line((const volatile uint32_t *)doorbell,
                                 expected, budget);
        if (why == PERL_KRT_WAIT_CHANGED) {
            perl_krt_poll_policy_hit(p);
            perl_krt_store_release(state, PERL_KRT_VEC_RUNNING);
            return PERL_KRT_WAIT_CHANGED;
        }
        perl_krt_poll_policy_miss(p);
    }
    else if (p) {
        p->misses++;                          /* parked without polling */
    }

    /* park: announce, then re-read.  The announce is seq_cst so that a
     * poster which misses it has bumped the doorbell before our re-read. */
    perl_krt_store_seq_cst(state, PERL_KRT_VEC_PARKED);
    for (;;) {
        uint64_t left = PERL_KRT_WAIT_FOREVER;
        if (perl_krt_load_seq_cst(doorbell) != expected) {
            why = PERL_KRT_WAIT_CHANGED;
            break;
        }
        if (deadline != PERL_KRT_WAIT_FOREVER) {
            uint64_t now = perl_krt_now_ns();
            if (now >= deadline) {
                why = PERL_KRT_WAIT_TIMEOUT;
                break;
            }
            left = deadline - now;
        }
        perl_krt_wait_u32_flags((volatile uint32_t *)doorbell, expected,
                                left, PERL_KRT_WAIT_SHARED);
    }
    perl_krt_store_release(state, PERL_KRT_VEC_RUNNING);
    return why;
}

/* The wait with the policy's learning: a wait that ended CHANGED teaches
 * the policy how long the round trip took.                               */
static inline perl_krt_wait_reason_t
perl_krt_db_wait(perl_krt_au32_t *doorbell, perl_krt_au32_t *state,
                 uint32_t expected, perl_krt_poll_policy_t *p,
                 uint64_t timeout_ns)
{
    const uint64_t t0 = p ? perl_krt_now_ns() : 0;
    perl_krt_wait_reason_t why =
        perl_krt_db_wait_raw(doorbell, state, expected, p, timeout_ns);
    if (p && why == PERL_KRT_WAIT_CHANGED)
        perl_krt_poll_policy_learn(p, perl_krt_now_ns() - t0);
    return why;
}

/* Wait for a post to vector idx after `expected` (the doorbell as read
 * before the caller looked for work).  Returns CHANGED or TIMEOUT.       */
static inline perl_krt_wait_reason_t
perl_krt_vec_wait(perl_krt_vectab_t *t, uint32_t idx, uint32_t expected,
                  perl_krt_poll_policy_t *p, uint64_t timeout_ns)
{
    perl_krt_vec_t *v = perl_krt_vec_at(t, idx);
    perl_krt_store_relaxed(&v->cpu, perl_krt_cpu_id());
    return perl_krt_db_wait(&v->doorbell, &v->state, expected, p, timeout_ns);
}

/* Wait for a post to any vector of word `word` after `expected`.  The
 * caller reads the word's doorbell, then takes the bits, then waits:
 *     d = doorbell(word); bits = pba_take(word); if (!bits) pba_wait(...);
 * so a post between the take and the wait changes the doorbell.          */
static inline perl_krt_wait_reason_t
perl_krt_pba_wait(perl_krt_vectab_t *t, uint32_t word, uint32_t expected,
                  perl_krt_poll_policy_t *p, uint64_t timeout_ns)
{
    perl_krt_pba_word_t *w = perl_krt_pba_at(t, word);
    return perl_krt_db_wait(&w->doorbell, &w->state, expected, p, timeout_ns);
}

/* The word's doorbell, for the read before take.                         */
static inline uint32_t
perl_krt_pba_doorbell(perl_krt_vectab_t *t, uint32_t word)
{
    return perl_krt_load_acquire(&perl_krt_pba_at(t, word)->doorbell);
}

/* Exchange the word's pending bits with 0: returns the bits taken.       */
static inline uint64_t
perl_krt_pba_take(perl_krt_vectab_t *t, uint32_t word)
{
    return perl_krt_exchange_acq_rel(&perl_krt_pba_at(t, word)->bits, 0);
}

/* 1 if vector idx has a pending bit.                                      */
static inline int
perl_krt_vec_pending(perl_krt_vectab_t *t, uint32_t idx)
{
    perl_krt_pba_word_t *w = perl_krt_pba_at(t, idx / PERL_KRT_PBA_BITS);
    return (int)((perl_krt_load_seq_cst(&w->bits)
                  >> (idx % PERL_KRT_PBA_BITS)) & 1u);
}

/* Mask or unmask a vector.  Unmasking wakes a waiter that parked while
 * the vector was masked (its post did not wake it).                      */
static inline void
perl_krt_vec_mask(perl_krt_vectab_t *t, uint32_t idx, int on)
{
    perl_krt_vec_t *v = perl_krt_vec_at(t, idx);
    perl_krt_pba_word_t *w = perl_krt_pba_at(t, idx / PERL_KRT_PBA_BITS);
    perl_krt_store_seq_cst(&v->mask, on ? 1u : 0u);
    if (!on) {
        if (perl_krt_load_seq_cst(&v->state) == PERL_KRT_VEC_PARKED)
            perl_krt_vec_wake(&v->doorbell);
        if (perl_krt_load_seq_cst(&w->state) == PERL_KRT_VEC_PARKED)
            perl_krt_vec_wake(&w->doorbell);
    }
}

/* ---- ownership ---------------------------------------------------------- */

/* Record that owner_pid waits on vector idx.  Returns the new generation,
 * which the peer keeps to check the claim later.                          */
static inline uint32_t
perl_krt_vec_claim(perl_krt_vectab_t *t, uint32_t idx, uint32_t owner_pid)
{
    perl_krt_vec_t *v = perl_krt_vec_at(t, idx);
    v->owner_pid = owner_pid;
    v->gen++;
    perl_krt_store_seq_cst(&v->cpu, -1);
    return v->gen;
}

/* 0 when the claim `gen` is still the owner of vector idx and the owner
 * is alive; -ESTALE when the vector was claimed again; -ESRCH when the
 * owner has exited (or was never claimed).                               */
static inline int
perl_krt_vec_owner_check(perl_krt_vectab_t *t, uint32_t idx, uint32_t gen)
{
    perl_krt_vec_t *v = perl_krt_vec_at(t, idx);
    if (v->gen != gen)
        return -ESTALE;
    if (v->owner_pid == 0)
        return -ESRCH;
    if (kill((pid_t)v->owner_pid, 0) != 0 && errno == ESRCH)
        return -ESRCH;
    return 0;
}

#endif /* PERL_KRT_VEC_H */

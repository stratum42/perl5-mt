/* The vector table inside one process.
 *
 *   A  mask: a masked post keeps its pending bit and does not wake a
 *      parked waiter; unmasking wakes it; the next wait sees the post.
 *   B  four posters, one waiter, 64 vectors, 10 million posts.  Each
 *      vector has one poster that waits for its pending bit to be taken
 *      before posting again, so every post is observed by exactly one
 *      take.  Checked per vector, and against the doorbell counts.  A
 *      lost wakeup shows up as a wait that times out.
 *   C  the poll-to-park race: the waiter never polls, one poster posts
 *      at random moments, a million times.  Every post is taken once. */
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "krt.h"
#include "krt_vec.h"
#include "krt_test.h"

#ifdef KRT_SAN
#  define TOTAL_POSTS  200000u
#  define PARK_POSTS   20000u
#else
#  define TOTAL_POSTS  10000000u
#  define PARK_POSTS   1000000u
#endif
#define NVEC       64u
#define NPOSTERS   4u
#define VEC_PER_P  16u
#define WAIT_NS    2000000000ull   /* 2 s: a lost wakeup times out       */

static perl_krt_vectab_t *tab;

static uint32_t
xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void
relax_or_yield(unsigned *n)
{
    perl_krt_cpu_relax();
    if ((++*n & 1023u) == 0)
        sched_yield();
}

/* ---- B: many posters, one waiter --------------------------------------- */

typedef struct { uint32_t first, count; } poster_b_t;

static void *
poster_b(void *a)
{
    poster_b_t *p = (poster_b_t *)a;
    uint32_t i;
    for (i = 0; i < p->count; i++) {
        uint32_t v = p->first + (i % VEC_PER_P);
        unsigned n = 0;
        while (perl_krt_vec_pending(tab, v))
            relax_or_yield(&n);
        perl_krt_vec_post(tab, v);
    }
    return NULL;
}

/* ---- C: the park race --------------------------------------------------- */

typedef struct { uint64_t wakes; } poster_c_t;

static void *
poster_c(void *a)
{
    poster_c_t *p = (poster_c_t *)a;
    uint32_t i, rnd = 0x9e3779b9u;
    unsigned n = 0;
    for (i = 0; i < PARK_POSTS; i++) {
        uint32_t spins;
        while (perl_krt_vec_pending(tab, 0))
            relax_or_yield(&n);
        p->wakes += (uint64_t)perl_krt_vec_post(tab, 0);
        spins = xorshift(&rnd) & 255u;          /* random moment, 0..255 */
        while (spins--)
            perl_krt_cpu_relax();
    }
    return NULL;
}

/* ---- A: the masked waiter ------------------------------------------------ */

typedef struct {
    uint32_t idx, expected;
    perl_krt_wait_reason_t reason;
    uint64_t t_ret;
    volatile int done;
} waiter_a_t;

static void *
waiter_a(void *a)
{
    waiter_a_t *w = (waiter_a_t *)a;
    perl_krt_poll_policy_t park;
    perl_krt_poll_policy_init(&park, 0, 0);     /* park at once */
    w->reason = perl_krt_vec_wait(tab, w->idx, w->expected, &park,
                                  WAIT_NS);
    w->t_ret = perl_krt_now_ns();
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    static uint64_t counts[NVEC];
    static uint32_t doorbell_of[NVEC];
    size_t len = perl_krt_vectab_size(NVEC);
    void *raw, *mem;
    uint64_t total = 0, sum_db = 0, taken;
    int failed = 0, v;
    uint32_t expected_per_vec = TOTAL_POSTS / NVEC;

    unsetenv("PERL_KRT_POLL_NS");
    raw = malloc(len + PERL_KRT_CACHE_LINE);
    if (!raw)
        return 1;
    mem = (void *)(((uintptr_t)raw + PERL_KRT_CACHE_LINE - 1)
                   & ~(uintptr_t)(PERL_KRT_CACHE_LINE - 1));
    tab = perl_krt_vectab_init(mem, len, NVEC);
    ok(tab != NULL, "table initialised, %u vectors, %zu bytes", NVEC, len);
    ok(perl_krt_vectab_attach(mem, len) == tab, "attach accepts our header");
    ok(perl_krt_vectab_attach(mem, 64) == NULL, "attach refuses a short buffer");

    /* ---- A ---- */
    {
        pthread_t th;
        waiter_a_t w = { 5, 0, 0, 0, 0 };
        uint32_t d0 = perl_krt_load_acquire(&perl_krt_vec_at(tab, 5)->doorbell);
        uint64_t t_unmask;
        w.expected = d0;
        perl_krt_vec_mask(tab, 5, 1);
        if (pthread_create(&th, NULL, waiter_a, &w) != 0)
            return 1;
        usleep(100000);                          /* let it park            */
        is_int(perl_krt_vec_post(tab, 5), 0, "masked post makes no wake call");
        ok(perl_krt_vec_pending(tab, 5) == 1, "masked post leaves its bit pending");
        usleep(100000);
        ok(__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) == 0,
           "the parked waiter stays asleep while masked");
        t_unmask = perl_krt_now_ns();
        perl_krt_vec_mask(tab, 5, 0);            /* wakes the parked waiter */
        pthread_join(th, NULL);
        is_int(w.reason, PERL_KRT_WAIT_CHANGED, "unmask wakes the waiter: CHANGED");
        ok(w.t_ret >= t_unmask && w.t_ret - t_unmask < 1000000000ull,
           "the waiter returns soon after the unmask");
        is_int(perl_krt_vec_wait(tab, 5, d0, NULL, 1000000ull), PERL_KRT_WAIT_CHANGED,
               "the next wait on the old doorbell sees the masked post");
        ok((perl_krt_pba_take(tab, 0) >> 5) & 1u, "the pending bit is still there to take");
        ok(perl_krt_vec_pending(tab, 5) == 0, "taking the word clears the bit");
    }

    /* ---- B ---- */
    {
        pthread_t th[NPOSTERS];
        poster_b_t pb[NPOSTERS];
        perl_krt_poll_policy_t pol;
        uint32_t p;
        perl_krt_poll_policy_init(&pol, PERL_KRT_POLL_DEFAULT_MIN_NS,
                                  PERL_KRT_POLL_DEFAULT_MAX_NS);
        for (p = 0; p < NPOSTERS; p++) {
            pb[p].first = p * VEC_PER_P;
            pb[p].count = TOTAL_POSTS / NPOSTERS;
            if (pthread_create(&th[p], NULL, poster_b, &pb[p]) != 0)
                return 1;
        }
        while (total < TOTAL_POSTS && !failed) {
            uint32_t d = perl_krt_pba_doorbell(tab, 0);
            uint64_t bits = perl_krt_pba_take(tab, 0);
            if (bits) {
                while (bits) {
                    counts[__builtin_ctzll(bits)]++;
                    total++;
                    bits &= bits - 1;
                }
                continue;
            }
            if (perl_krt_pba_wait(tab, 0, d, &pol, WAIT_NS) != PERL_KRT_WAIT_CHANGED)
                failed = 1;
        }
        for (p = 0; p < NPOSTERS; p++)
            pthread_join(th[p], NULL);
        printf("# B: %llu posts taken, polled %u, parked %u (adaptive, cur %llu ns)\n",
               (unsigned long long)total, pol.hits, pol.misses,
               (unsigned long long)pol.cur_ns);
        is_int(failed, 0, "no wait timed out: no lost wakeup");
        is_int(total, TOTAL_POSTS, "%u posts, each taken exactly once in total", TOTAL_POSTS);
        for (v = 0; v < (int)NVEC; v++) {
            if (counts[v] != expected_per_vec)
                break;
        }
        ok(v == (int)NVEC, "every vector taken %u times", expected_per_vec);
        for (v = 0; v < (int)NVEC; v++) {
            doorbell_of[v] = perl_krt_load_acquire(&perl_krt_vec_at(tab, v)->doorbell);
            sum_db += doorbell_of[v];
        }
        /* part A posted once more, to vector 5 (masked) */
        is_int(sum_db, TOTAL_POSTS + 1, "vector doorbells add up to the posts");
        is_int(perl_krt_load_acquire(&perl_krt_pba_at(tab, 0)->doorbell), TOTAL_POSTS + 1,
               "word doorbell counts every post");
        taken = perl_krt_pba_take(tab, 0);
        is_int(taken, 0, "no bit left pending after the last take");
    }

    /* ---- C ---- */
    {
        pthread_t th;
        poster_c_t pc = { 0 };
        perl_krt_poll_policy_t park;
        uint64_t got = 0;
        int lost = 0;
        perl_krt_poll_policy_init(&park, 0, 0);
        if (pthread_create(&th, NULL, poster_c, &pc) != 0)
            return 1;
        while (got < PARK_POSTS) {
            uint32_t d = perl_krt_load_acquire(&perl_krt_vec_at(tab, 0)->doorbell);
            uint64_t bits = perl_krt_pba_take(tab, 0);
            if (bits) {
                if (bits != 1u) {
                    lost = 1;
                    break;
                }
                got++;
                continue;
            }
            if (perl_krt_vec_wait(tab, 0, d, &park, WAIT_NS) != PERL_KRT_WAIT_CHANGED) {
                lost = 1;
                break;
            }
        }
        pthread_join(th, NULL);
        printf("# C: %llu posts, %llu wake calls, %u parks\n",
               (unsigned long long)got, (unsigned long long)pc.wakes, park.misses);
        is_int(lost, 0, "park race: every post taken once, no lost wakeup");
        is_int(got, PARK_POSTS, "%u posts through the park race", PARK_POSTS);
        ok(pc.wakes > 0, "the parked waiter was woken by posts");
    }

    free(raw);
    done_testing();
}

/* perl_krt_wait_line: the wakeup latency of the tier this CPU selected,
 * and the budget path.  A poster thread stores after a delay; the waiter
 * spins with wait_line and must return CHANGED soon after the store.  The
 * budget path must return BUDGET within twice the budget. */
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "krt.h"
#include "krt_wait.h"
#include "krt_test.h"

#define ITER        200
#define BUDGET_NS   1000000ull     /* 1 ms */
#define BUDGET_ITER 50

static uint32_t word;              /* the word waited on                  */
static uint64_t stored_at;         /* set before the release store        */

typedef struct { uint32_t value; uint64_t delay_ns; } poster_arg_t;

static void *poster(void *a)
{
    poster_arg_t *p = (poster_arg_t *)a;
    struct timespec ts;
    ts.tv_sec = (time_t)(p->delay_ns / 1000000000ull);
    ts.tv_nsec = (long)(p->delay_ns % 1000000000ull);
    nanosleep(&ts, NULL);
    stored_at = perl_krt_now_ns();
    __atomic_store_n(&word, p->value, __ATOMIC_RELEASE);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    static uint64_t lat[ITER], el[BUDGET_ITER];
    uint32_t cur = 0;
    int i, bad = 0, over = 0;
    uint64_t max_lat = 0;
    const char *tier;

    tier = perl_krt_wait_tier();          /* calibrates the tick counter */
    ok(tier && *tier, "tier selected: %s", tier);
    printf("# tier %s on %s\n", tier, perl_krt_platform());

    /* the value already differs: returns CHANGED at once, budget 0 */
    is_int(perl_krt_wait_line(&word, cur - 1, 0), PERL_KRT_WAIT_CHANGED,
           "changed before the first look, zero budget");

    for (i = 0; i < ITER; i++) {
        pthread_t th;
        poster_arg_t pa = { cur + 1, 200000u + (uint64_t)(i % 7) * 50000u };
        perl_krt_wait_reason_t r;
        uint64_t t1;
        if (pthread_create(&th, NULL, poster, &pa) != 0)
            return 1;
        r = perl_krt_wait_line(&word, cur, 100000000ull);   /* 100 ms cap */
        t1 = perl_krt_now_ns();
        pthread_join(th, NULL);
        if (r != PERL_KRT_WAIT_CHANGED || word != cur + 1)
            bad++;
        else {
            lat[i] = t1 - stored_at;
            if (lat[i] > max_lat)
                max_lat = lat[i];
        }
        cur++;
    }
    is_int(bad, 0, "%d wakeups returned CHANGED with the new value", ITER);
    {
        uint64_t sorted[ITER];
        memcpy(sorted, lat, sizeof sorted);
        qsort(sorted, ITER, sizeof sorted[0], cmp_u64);
        printf("# wakeup latency over %d stores: median %llu ns, p90 %llu ns, "
               "max %llu ns\n", ITER,
               (unsigned long long)sorted[ITER / 2],
               (unsigned long long)sorted[ITER * 9 / 10],
               (unsigned long long)max_lat);
    }

    /* budget path: the word never changes */
    for (i = 0; i < BUDGET_ITER; i++) {
        uint64_t t0 = perl_krt_now_ns();
        perl_krt_wait_reason_t r = perl_krt_wait_line(&word, cur, BUDGET_NS);
        uint64_t t1 = perl_krt_now_ns();
        if (r != PERL_KRT_WAIT_BUDGET)
            bad++;
        el[i] = t1 - t0;
        if (el[i] > 2 * BUDGET_NS)
            over++;
    }
    is_int(bad, 0, "budget path returns BUDGET every time");
    {
        uint64_t sorted[BUDGET_ITER];
        memcpy(sorted, el, sizeof sorted);
        qsort(sorted, BUDGET_ITER, sizeof sorted[0], cmp_u64);
        printf("# budget %llu ns: median elapsed %llu ns, max %llu ns\n",
               (unsigned long long)BUDGET_NS,
               (unsigned long long)sorted[BUDGET_ITER / 2],
               (unsigned long long)sorted[BUDGET_ITER - 1]);
        ok(sorted[BUDGET_ITER / 2] <= 2 * BUDGET_NS,
           "median budget elapsed %llu ns within 2x the budget",
           (unsigned long long)sorted[BUDGET_ITER / 2]);
        /* the tick budget is calibrated against the monotonic clock, so
         * allow the calibration error (a few per cent) */
        ok(sorted[BUDGET_ITER / 2] >= BUDGET_NS * 9 / 10,
           "median budget elapsed is not shorter than 90%% of the budget");
    }
    is_int(over, 0, "no budget overran 2x (%d of %d)", over, BUDGET_ITER);
    done_testing();
}

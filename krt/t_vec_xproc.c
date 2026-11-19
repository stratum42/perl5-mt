/* The vector table across processes.  The table lives in a perl_krt_shm_t;
 * a forked child is the server.  The parent posts a request (vector REQ,
 * word 0) and waits for the reply (vector REP, word 1); the child waits
 * for REQ, takes its pending bit, and posts REP.  Every round trip checks
 * that exactly the expected pending bit was taken.
 *
 * Phases:
 *   parked    PERL_KRT_POLL_NS=0: the waiter parks at once (the bridge of
 *             fiber runtime)
 *   adaptive  the default policy, 2 us to 50 us
 *   death     the child exits after 99 of 100 round trips: the parent's
 *             wait for the 100th returns TIMEOUT, and the owner check on
 *             REQ fails with -ESRCH once the child is reaped
 *   smt       parent pinned to cpu 0, child pinned to its SMT sibling
 *             (skipped when cpu 0 has none)
 *
 * Every phase prints the median and p99 round trip, for the README.     */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "krt.h"
#include "krt_vec.h"
#include "krt_test.h"

#define NVEC  128u
#define REQ   0u            /* word 0, bit 0                              */
#define REP   64u           /* word 1, bit 0                              */

#ifdef KRT_SAN
#  define ROUNDS 20000u
#else
#  define ROUNDS 1000000u
#endif
#define WAIT_NS       5000000000ull     /* a healthy round trip never gets here */
#define DEATH_WAIT_NS 200000000ull

typedef struct {
    void     *base;
    size_t    len;
    uint32_t  rounds;      /* round trips the child serves, then it exits */
    int       pin;         /* CPU to pin the child to, or -1             */
} child_arg_t;

typedef struct {
    const char *name;
    const char *poll_ns;   /* PERL_KRT_POLL_NS, or NULL for the default  */
    uint32_t    rounds;    /* round trips timed in the parent            */
    uint32_t    child_rounds;
    uint64_t    wait_ns;
    int         pin_parent;
    int         pin_child;
    int         death;     /* expect the child to die mid-wait           */
} phase_t;

typedef struct {
    uint32_t median_ns, p99_ns;
    uint32_t n;            /* round trips completed                      */
    uint32_t timeouts;
    uint32_t bad;          /* a wrong pending bit or a child failure     */
    int      child_status;
    int      owner_check;  /* death phase: owner_check on REQ            */
    uint32_t polled, parked;
} phase_result_t;

static int
child_fn(void *a)
{
    child_arg_t *arg = (child_arg_t *)a;
    perl_krt_vectab_t *t = perl_krt_vectab_attach(arg->base, arg->len);
    perl_krt_poll_policy_t pol;
    uint32_t seen = 0, i;

    if (!t)
        return 2;
    if (arg->pin >= 0 && perl_krt_cpu_pin(arg->pin) != 0)
        return 3;
    perl_krt_poll_policy_init(&pol, PERL_KRT_POLL_DEFAULT_MIN_NS,
                              PERL_KRT_POLL_DEFAULT_MAX_NS);
    perl_krt_vec_claim(t, REQ, (uint32_t)getpid());
    for (i = 0; i < arg->rounds; i++) {
        if (perl_krt_vec_wait(t, REQ, seen, &pol, WAIT_NS)
            != PERL_KRT_WAIT_CHANGED)
            return 4;
        seen = perl_krt_load_acquire(&perl_krt_vec_at(t, REQ)->doorbell);
        if (perl_krt_pba_take(t, 0) != 1u)
            return 5;
        perl_krt_vec_post(t, REP);
    }
    return 0;
}

static int
cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int
run_phase(perl_krt_shm_t *shm, const phase_t *ph, phase_result_t *res)
{
    perl_krt_vectab_t *t;
    perl_krt_comp_t comp;
    perl_krt_poll_policy_t pol;
    child_arg_t arg;
    uint32_t *lat;
    uint32_t seen = 0, i;
    int rc;

    memset(res, 0, sizeof *res);
    if (ph->poll_ns)
        setenv("PERL_KRT_POLL_NS", ph->poll_ns, 1);
    else
        unsetenv("PERL_KRT_POLL_NS");

    t = perl_krt_vectab_init(shm->base, shm->len, NVEC);
    if (!t)
        return -1;
    perl_krt_poll_policy_init(&pol, PERL_KRT_POLL_DEFAULT_MIN_NS,
                              PERL_KRT_POLL_DEFAULT_MAX_NS);
    if (ph->pin_parent >= 0 && perl_krt_cpu_pin(ph->pin_parent) != 0)
        return -2;
    perl_krt_vec_claim(t, REP, (uint32_t)getpid());

    arg.base = shm->base;
    arg.len = shm->len;
    arg.rounds = ph->child_rounds;
    arg.pin = ph->pin_child;
    fflush(stdout);                 /* the child must not inherit output */
    rc = perl_krt_comp_spawn(&comp, child_fn, &arg, shm, 1);
    if (rc)
        return rc;

    lat = (uint32_t *)malloc(sizeof *lat * ph->rounds);
    if (!lat)
        return -ENOMEM;
    for (i = 0; i < ph->rounds; i++) {
        uint64_t t0, t1;
        perl_krt_wait_reason_t r;
        t0 = perl_krt_now_ns();
        perl_krt_vec_post(t, REQ);
        r = perl_krt_vec_wait(t, REP, seen, &pol, ph->wait_ns);
        t1 = perl_krt_now_ns();
        if (r != PERL_KRT_WAIT_CHANGED) {
            res->timeouts++;
            break;
        }
        seen = perl_krt_load_acquire(&perl_krt_vec_at(t, REP)->doorbell);
        if (perl_krt_pba_take(t, 1) != 1u) {
            res->bad++;
            break;
        }
        lat[res->n++] = (t1 - t0) > UINT32_MAX ? UINT32_MAX : (uint32_t)(t1 - t0);
    }
    perl_krt_comp_reap(&comp, &res->child_status);
    if (ph->death)
        res->owner_check = perl_krt_vec_owner_check(t, REQ,
                                perl_krt_vec_at(t, REQ)->gen);
    res->polled = pol.hits;
    res->parked = pol.misses;

    if (res->n) {
        qsort(lat, res->n, sizeof *lat, cmp_u32);
        res->median_ns = lat[res->n / 2];
        res->p99_ns = lat[res->n * 99u / 100u];
    }
    free(lat);
    return 0;
}

int main(void)
{
    static const phase_t phases[] = {
        { "parked",   "0",  ROUNDS, ROUNDS, WAIT_NS,       -1, -1, 0 },
        { "adaptive", NULL, ROUNDS, ROUNDS, WAIT_NS,       -1, -1, 0 },
        { "death",    NULL, 100,    99,     DEATH_WAIT_NS, -1, -1, 1 },
    };
    perl_krt_shm_t shm;
    size_t need = perl_krt_vectab_size(NVEC);
    size_t i;
    int rc, sib[8], nsib;

    rc = perl_krt_shm_create(&shm, need, 0);
    if (rc == PERL_KRT_ENOSYS)
        skip_all("compartments not implemented on %s", perl_krt_platform());
    is_int(rc, 0, "shm for the table");
    printf("# tier %s, %d cpus, numa node of cpu 0: %d\n",
           perl_krt_wait_tier(), perl_krt_cpu_count(), perl_krt_numa_node(0));

    for (i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        const phase_t *ph = &phases[i];
        phase_result_t res;
        int r = run_phase(&shm, ph, &res);
        printf("# %-9s median %7u ns  p99 %7u ns  round trips %u  polled %u  parked %u\n",
               ph->name, res.median_ns, res.p99_ns, res.n, res.polled, res.parked);
        is_int(r, 0, "%s: phase ran", ph->name);
        if (ph->death) {
            is_int(res.timeouts, 1, "%s: the wait for the dead peer's reply timed out", ph->name);
            is_int(res.n, ph->rounds - 1, "%s: every earlier round trip completed", ph->name);
            is_int(res.owner_check, -ESRCH, "%s: owner check on REQ fails with ESRCH", ph->name);
            is_int(res.child_status, 0, "%s: the child exited cleanly", ph->name);
        } else {
            is_int(res.timeouts, 0, "%s: no round trip timed out", ph->name);
            is_int(res.bad, 0, "%s: every round trip took exactly its own bit", ph->name);
            is_int(res.n, ph->rounds, "%s: %u round trips", ph->name, ph->rounds);
            is_int(res.child_status, 0, "%s: the child served every round", ph->name);
        }
    }

    nsib = perl_krt_smt_siblings(0, sib, 8);
    if (nsib > 0) {
        phase_t smt = { "smt-paired", NULL, ROUNDS, ROUNDS, WAIT_NS, 0, sib[0], 0 };
        phase_result_t res;
        int r;
        printf("# cpu 0 has %d SMT sibling(s); child on cpu %d\n", nsib, sib[0]);
        r = run_phase(&shm, &smt, &res);
        printf("# %-9s median %7u ns  p99 %7u ns  round trips %u  polled %u  parked %u\n",
               smt.name, res.median_ns, res.p99_ns, res.n, res.polled, res.parked);
        is_int(r, 0, "smt-paired: phase ran");
        is_int(res.bad + res.timeouts, 0, "smt-paired: every round trip good");
        is_int(res.child_status, 0, "smt-paired: the child served every round");
    } else {
        printf("# cpu 0 has no SMT sibling (siblings() = %d): the smt-paired row is not run\n",
               nsib);
    }

    perl_krt_shm_destroy(&shm);
    done_testing();
}

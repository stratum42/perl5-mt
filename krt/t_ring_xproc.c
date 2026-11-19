/* rings between processes: a request ring and a reply ring in a shared
 * segment, PERL_KRT_RING_SHARED, a compartment serving ping-pong.  Without
 * the shared futex/ulock every round trip would last the full wait
 * timeout, so the elapsed time is the assertion. */
#include <errno.h>
#include <unistd.h>
#include "krt.h"
#include "krt_ring.h"
#include "krt_test.h"

#define ROUNDS 2000
#define CAP 64

typedef struct { perl_krt_shm_t *shm; } arg_t;

static perl_krt_mpsc_t *req_ring(void *base) { return (perl_krt_mpsc_t *)base; }
static perl_krt_spsc_t *rep_ring(void *base) { return (perl_krt_spsc_t *)((char *)base + 32768); }

static int child_fn(void *a)
{
    arg_t *arg = (arg_t *)a;
    perl_krt_mpsc_t *req = perl_krt_mpsc_attach(req_ring(arg->shm->base));
    perl_krt_spsc_t *rep = perl_krt_spsc_attach(rep_ring(arg->shm->base));
    uint32_t n = 0;
    if (!req || !rep) return 2;
    for (;;) {
        uint64_t v;                      /* items are whole 8-byte slots */
        uint32_t pub = perl_krt_mpsc_seq(req);
        if (perl_krt_mpsc_pop(req, &v, sizeof v) != 0) {
            perl_krt_mpsc_wait(req, pub, 1000000000ull);
            continue;
        }
        if (v == UINT64_MAX) return 0;
        v ^= 0x5a5a5a5au;
        while (perl_krt_spsc_push(rep, &v) != 0)
            ;
        n++;
    }
}

int main(void)
{
    perl_krt_shm_t shm;
    perl_krt_comp_t comp;
    arg_t arg;
    perl_krt_mpsc_t *req;
    perl_krt_spsc_t *rep;
    uint64_t t0, t1;
    int rc, status = -1, bad = 0;
    uint32_t i;

    rc = perl_krt_shm_create(&shm, 65536, 0);
    if (rc == PERL_KRT_ENOSYS)
        skip_all("compartments not implemented on %s", perl_krt_platform());
    is_int(rc, 0, "shm");
    req = perl_krt_mpsc_init_flags(req_ring(shm.base), 32768, CAP, sizeof(uint64_t),
                                   PERL_KRT_RING_SHARED);
    rep = perl_krt_spsc_init_flags(rep_ring(shm.base), 32768, CAP, sizeof(uint64_t),
                                   PERL_KRT_RING_SHARED);
    ok(req && rep, "shared rings initialised");
    arg.shm = &shm;
    is_int(perl_krt_comp_spawn(&comp, child_fn, &arg, &shm, 1), 0, "spawn");

    t0 = perl_krt_now_ns();
    for (i = 0; i < ROUNDS && !bad; i++) {
        uint64_t v = i, r = 0;
        while (perl_krt_mpsc_push(req, &v, sizeof v) != 0)
            ;
        for (;;) {
            uint32_t pub = perl_krt_spsc_seq(rep);
            if (perl_krt_spsc_pop(rep, &r) == 0)
                break;
            perl_krt_spsc_wait(rep, pub, 200000000ull);
            if (perl_krt_comp_reap_flags(&comp, &status, PERL_KRT_REAP_NOHANG) != -EAGAIN) {
                bad = ROUNDS;             /* the child died: do not hang */
                break;
            }
        }
        if (r != ((uint64_t)i ^ 0x5a5a5a5au))
            bad++;
    }
    t1 = perl_krt_now_ns();
    is_int(bad, 0, "%d ping-pongs answered correctly", ROUNDS);
    ok(t1 - t0 < 5000000000ull, "%d cross-process round trips in %.3f s "
       "(wakeups cross the fork)", ROUNDS, (double)(t1 - t0) / 1e9);
    {
        uint64_t stop = UINT64_MAX;
        while (perl_krt_mpsc_push(req, &stop, sizeof stop) != 0)
            ;
    }
    is_int(perl_krt_comp_reap(&comp, &status), 0, "reap");
    is_int(status, 0, "child exit status");
    perl_krt_shm_destroy(&shm);
    done_testing();
}

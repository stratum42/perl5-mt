/* notify: cross-thread wakeups into a draining context; Linux also from a
 * compartment (eventfd + shm ring survive fork). */
#include <pthread.h>
#include <unistd.h>
#include "krt.h"
#include "krt_atomic.h"
#include "krt_test.h"

#define NN 2000
static perl_krt_aio_t ctx;

static void *notifier(void *a)
{
    uint64_t base = (uint64_t)(uintptr_t)a, i;
    for (i = 0; i < NN; i++)
        while (perl_krt_aio_notify(&ctx, base + i) == -EAGAIN)
            perl_krt_cpu_relax();
    return NULL;
}

static int comp_notifier(void *a)
{
    (void)a;
    return perl_krt_aio_notify(&ctx, 0xC0FFEE) == 0 ? 0 : 9;
}

int main(void)
{
    pthread_t t[2];
    perl_krt_cqe_t c[64];
    uint64_t seen = 0, t0, dt;
    int rc, tries = 0, n, i, nonotify = 0;

    rc = perl_krt_aio_init(&ctx, 16, NULL);
    if (rc == PERL_KRT_ENOSYS) skip_all("aio not available on %s", perl_krt_platform());
    is_int(rc, 0, "aio_init");

    /* a blocking drain must wake on notify */
    pthread_create(&t[0], NULL, notifier, (void *)(uintptr_t)1000);
    pthread_create(&t[1], NULL, notifier, (void *)(uintptr_t)5000);
    while (seen < 2 * NN && tries++ < 100000) {
        n = perl_krt_aio_drain(&ctx, c, 64, PERL_KRT_WAIT_FOREVER);
        if (n < 0) break;
        for (i = 0; i < n; i++) {
            if (c[i].flags & PERL_KRT_CQE_NOTIFY) seen++; else nonotify++;
        }
    }
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    is_int(seen, 2 * NN, "received %d notifications", 2 * NN);
    is_int(nonotify, 0, "no stray completions");

    t0 = perl_krt_now_ns();
    n = perl_krt_aio_drain(&ctx, c, 64, 20000000ull);
    dt = perl_krt_now_ns() - t0;
    is_int(n, 0, "quiet after all were consumed");
    ok(dt >= 10000000ull, "timed drain waited (%.1f ms)", dt / 1e6);

#if defined(__linux__)
    {
        perl_krt_comp_t comp;
        int status = -1;
        rc = perl_krt_comp_spawn(&comp, comp_notifier, NULL, NULL, 0);
        is_int(rc, 0, "spawn a compartment that notifies");
        n = 0; tries = 0; seen = 0;
        while (!seen && tries++ < 20) {
            int m = perl_krt_aio_drain(&ctx, c, 64, 500000000ull);
            for (i = 0; i < m; i++) if (c[i].user_data == 0xC0FFEE) seen = 1;
        }
        is_int(seen, 1, "notification from the compartment woke the host");
        is_int(perl_krt_comp_reap(&comp, &status), 0, "reap");
        is_int(status, 0, "compartment notify succeeded");
    }
#else
    (void)comp_notifier;
#endif
    is_int(perl_krt_aio_close(&ctx), 0, "aio_close");
    done_testing();
}

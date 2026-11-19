/* wait/wake: blocking wait woken by another thread, timeout, bridge. */
#include <pthread.h>
#include "krt.h"
#include "krt_atomic.h"
#include "krt_test.h"

static perl_krt_au32_t word;
static perl_krt_au32_t woken;

static void *waiter(void *a)
{
    (void)a;
    while (perl_krt_load_seq_cst(&word) == 0)
        perl_krt_wait_u32((volatile uint32_t *)&word, 0, PERL_KRT_WAIT_FOREVER);
    perl_krt_fetch_add_acq_rel(&woken, 1);
    return NULL;
}

int main(void)
{
    pthread_t t[4];
    int i;
    atomic_init(&word, 0);
    uint64_t t0, dt;

    t0 = perl_krt_now_ns();
    perl_krt_wait_u32((volatile uint32_t *)&word, 0, 20000000ull);          /* 20 ms timeout */
    dt = perl_krt_now_ns() - t0;
    ok(dt >= 15000000ull && dt < 2000000000ull, "timed wait returned after %.1f ms", dt / 1e6);

    t0 = perl_krt_now_ns();
    perl_krt_wait_u32((volatile uint32_t *)&word, 1, PERL_KRT_WAIT_FOREVER);  /* value mismatch: no block */
    dt = perl_krt_now_ns() - t0;
    ok(dt < 1000000000ull, "wait with mismatched expected returns immediately");

    atomic_init(&woken, 0);
    for (i = 0; i < 4; i++) pthread_create(&t[i], NULL, waiter, NULL);
    {
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    is_int(perl_krt_load_acquire(&woken), 0, "waiters blocked while word == 0");
    perl_krt_store_seq_cst(&word, 1);
    perl_krt_wake_u32((volatile uint32_t *)&word, UINT32_MAX);
    for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
    is_int(perl_krt_load_acquire(&woken), 4, "all four waiters woken");
    ok(perl_krt_cpu_id() >= -1, "cpu_id = %d", perl_krt_cpu_id());
    ok(perl_krt_cpu_count() >= 1, "cpu_count = %d", perl_krt_cpu_count());
    ok(perl_krt_page_size() >= 4096, "page size %zu", perl_krt_page_size());
    done_testing();
}

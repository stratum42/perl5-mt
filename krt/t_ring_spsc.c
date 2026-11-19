/* SPSC ring: wrap-around, full/empty, 2-thread throughput, bridge wait. */
#include <pthread.h>
#include <stdlib.h>
#include "krt.h"
#include "krt_ring.h"
#include "krt_test.h"

#define N 5000000u
typedef struct { uint64_t seq; uint64_t x; } msg_t;

static perl_krt_spsc_t *ring;
static uint64_t consumed_sum;
static uint32_t consumed_n, out_of_order;

static void *producer(void *a)
{
    uint64_t i;
    (void)a;
    for (i = 0; i < N; i++) {
        msg_t m = { i, i * 3 };
        while (perl_krt_spsc_push(ring, &m) == -EAGAIN)
            perl_krt_cpu_relax();
    }
    return NULL;
}

static void *consumer(void *a)
{
    uint64_t expect = 0;
    (void)a;
    while (consumed_n < N) {
        msg_t m;
        if (perl_krt_spsc_pop(ring, &m) == 0) {
            if (m.seq != expect) out_of_order++;
            expect = m.seq + 1;
            consumed_sum += m.x;
            consumed_n++;
        }
        else {
            uint32_t s = perl_krt_spsc_seq(ring);
            if (perl_krt_spsc_count(ring) == 0)
                perl_krt_spsc_wait(ring, s, 1000000ull);
        }
    }
    return NULL;
}

int main(void)
{
    size_t bytes = perl_krt_spsc_bytes(8, sizeof(msg_t));
    void *mem = malloc(bytes);
    msg_t m, o;
    int i;
    pthread_t pt, ct;
    uint64_t want = 0, k;

    ring = perl_krt_spsc_init(mem, bytes, 8, sizeof(msg_t));
    ok(ring != NULL, "init cap 8");
    ok(perl_krt_spsc_init(mem, bytes, 6, sizeof(msg_t)) == NULL, "non power-of-two refused");
    ring = perl_krt_spsc_init(mem, bytes, 8, sizeof(msg_t));
    is_int(perl_krt_spsc_pop(ring, &o), -EAGAIN, "pop on empty");
    for (i = 0; i < 8; i++) { m.seq = i; m.x = i; is_int(perl_krt_spsc_push(ring, &m), 0, "push %d", i); }
    m.seq = 99;
    is_int(perl_krt_spsc_push(ring, &m), -EAGAIN, "push on full");
    is_int(perl_krt_spsc_count(ring), 8, "count 8");
    for (i = 0; i < 8; i++) { perl_krt_spsc_pop(ring, &o); if (o.seq != (uint64_t)i) break; }
    is_int(i, 8, "FIFO order through a wrap");
    for (i = 0; i < 20; i++) { m.seq = i; perl_krt_spsc_push(ring, &m); perl_krt_spsc_pop(ring, &o); }
    is_int(o.seq, 19, "push/pop interleaved across wrap");
    ok(perl_krt_spsc_attach(mem) == ring, "attach finds the magic");

    free(mem);
    bytes = perl_krt_spsc_bytes(1024, sizeof(msg_t));
    mem = malloc(bytes);
    ring = perl_krt_spsc_init(mem, bytes, 1024, sizeof(msg_t));
    pthread_create(&ct, NULL, consumer, NULL);
    pthread_create(&pt, NULL, producer, NULL);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    for (k = 0; k < N; k++) want += k * 3;
    is_int(consumed_n, N, "consumed %u messages", N);
    is_int(out_of_order, 0, "no reordering");
    ok(consumed_sum == want, "checksum matches");
    free(mem);
    done_testing();
}

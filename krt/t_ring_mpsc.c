/* MPSC ring: 4 producers / 1 consumer, per-producer ordering, bridge wait. */
#include <pthread.h>
#include <stdlib.h>
#include "krt.h"
#include "krt_ring.h"
#include "krt_test.h"

#define P 4
#define PER 2500000u
typedef struct { uint32_t who; uint32_t seq; uint64_t x; } msg_t;

static perl_krt_mpsc_t *ring;
static uint32_t next_expected[P];
static uint32_t bad_order, total;
static uint64_t sum;

static void *producer(void *a)
{
    uint32_t who = (uint32_t)(uintptr_t)a, i;
    for (i = 0; i < PER; i++) {
        uint32_t pos = 0; void *d = NULL;
        while (perl_krt_mpsc_claim(ring, &pos, &d) == -EAGAIN)
            perl_krt_cpu_relax();
        ((msg_t *)d)->who = who; ((msg_t *)d)->seq = i; ((msg_t *)d)->x = i;
        perl_krt_mpsc_publish(ring, pos);
    }
    return NULL;
}

static void *consumer(void *a)
{
    (void)a;
    while (total < P * PER) {
        msg_t m;
        if (perl_krt_mpsc_pop(ring, &m, sizeof m) == 0) {
            if (m.who >= P || m.seq != next_expected[m.who]) bad_order++;
            else next_expected[m.who]++;
            sum += m.x;
            total++;
        }
        else {
            uint32_t s = perl_krt_mpsc_seq(ring);
            if (perl_krt_mpsc_count(ring) == 0)
                perl_krt_mpsc_wait(ring, s, 1000000ull);
        }
    }
    return NULL;
}

int main(void)
{
    size_t bytes = perl_krt_mpsc_bytes(4, sizeof(msg_t));
    void *mem = malloc(bytes);
    msg_t m = {0, 0, 0}, o;
    uint32_t pos = 0; void *d = NULL;
    char big[200];
    int i;
    pthread_t pt[P], ct;
    uint64_t want = 0; uint32_t k;

    ring = perl_krt_mpsc_init(mem, bytes, 4, sizeof(msg_t));
    ok(ring != NULL, "init cap 4");
    is_int(perl_krt_mpsc_pop(ring, &o, sizeof o), -EAGAIN, "pop on empty");
    for (i = 0; i < 4; i++) { m.seq = i; is_int(perl_krt_mpsc_push(ring, &m, sizeof m), 0, "push %d", i); }
    is_int(perl_krt_mpsc_claim(ring, &pos, &d), -EAGAIN, "claim on full");
    is_int(perl_krt_mpsc_pop(ring, &o, sizeof o), 0, "pop");
    is_int(o.seq, 0, "FIFO");
    is_int(perl_krt_mpsc_claim(ring, &pos, &d), 0, "claim after pop frees a slot");
    ((msg_t *)d)->seq = 4;
    /* unpublished slot must not be visible: pop returns the next published */
    is_int(perl_krt_mpsc_pop(ring, &o, sizeof o), 0, "pop next published");
    is_int(o.seq, 1, "unpublished claim does not block earlier items");
    perl_krt_mpsc_publish(ring, pos);
    for (i = 0; i < 3; i++) perl_krt_mpsc_pop(ring, &o, sizeof o);
    is_int(o.seq, 4, "published claim arrives in claim order");
    memset(big, 0, sizeof big);
    is_int(perl_krt_mpsc_push(ring, big, sizeof big), -EMSGSIZE, "oversized push refused");
    free(mem);

    bytes = perl_krt_mpsc_bytes(1024, sizeof(msg_t));
    mem = malloc(bytes);
    ring = perl_krt_mpsc_init(mem, bytes, 1024, sizeof(msg_t));
    pthread_create(&ct, NULL, consumer, NULL);
    for (i = 0; i < P; i++) pthread_create(&pt[i], NULL, producer, (void *)(uintptr_t)i);
    for (i = 0; i < P; i++) pthread_join(pt[i], NULL);
    pthread_join(ct, NULL);
    for (k = 0; k < PER; k++) want += (uint64_t)k * P;
    is_int(total, P * PER, "consumed %u messages from %d producers", P * PER, P);
    is_int(bad_order, 0, "per-producer order preserved");
    ok(sum == want, "checksum matches");
    free(mem);
    done_testing();
}

/*    krt_ring.h
 *
 *    Lock-free bounded rings for perl5-mt, header-only and position
 *    independent: a ring is laid out inside caller-provided memory (which
 *    may be a perl_krt_shm_t segment mapped at different addresses in
 *    different processes) and every reference inside it is an offset.
 *
 *      perl_krt_spsc_*   single producer / single consumer
 *      perl_krt_mpsc_*   multi producer / single consumer, two-phase:
 *                        claim a slot, write it, publish it
 *
 *    Both carry a wakeup bridge so a consumer can sleep in
 *    perl_krt_wait_u32 until a producer publishes.
 *
 *    Capacities are powers of two; items are copied by value (slot_size
 *    bytes).  Positions are 32-bit and wrap; the ring works as long as
 *    capacity <= 2^31.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_RING_H
#define PERL_KRT_RING_H

#include <errno.h>
#include <string.h>
#include "krt.h"
#include "krt_atomic.h"

#define PERL_KRT_RING_MAGIC_SPSC  0x53505343u  /* 'SPSC' */
#define PERL_KRT_RING_MAGIC_MPSC  0x4d505343u  /* 'MPSC' */

/* ---- common wakeup bridge ---------------------------------------------- */

/* A ring whose producer and consumer are different processes (a worker
 * and an XS compartment) is initialised with PERL_KRT_RING_SHARED: its
 * bridge then uses the shared futex / ulock, since the process-private
 * form is keyed by the address space and never crosses a fork. */
#define PERL_KRT_RING_SHARED  0x1u

typedef struct perl_krt_ring_bridge {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t pub_seq;  /* bumped per publish */
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t sleepers; /* consumer parked    */
    uint32_t flags;                                  /* PERL_KRT_RING_*    */
} perl_krt_ring_bridge_t;

static inline uint32_t
perl_krt_bridge_wait_flags(const perl_krt_ring_bridge_t *b)
{
    return (b->flags & PERL_KRT_RING_SHARED) ? PERL_KRT_WAIT_SHARED : 0;
}

static inline void
perl_krt_bridge_publish(perl_krt_ring_bridge_t *b)
{
    /* seq_cst pairs with the consumer's seq_cst store of `sleepers` so
     * that at least one side observes the other (Dekker) */
    perl_krt_fetch_add_seq_cst(&b->pub_seq, 1);
    if (perl_krt_load_seq_cst(&b->sleepers))
        perl_krt_wake_u32_flags((volatile uint32_t *)&b->pub_seq, UINT32_MAX,
                                perl_krt_bridge_wait_flags(b));
}

/* ---- SPSC --------------------------------------------------------------- */

typedef struct perl_krt_spsc {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t magic;
    uint32_t cap;
    uint32_t mask;
    uint32_t slot_size;
    uint32_t data_off;            /* bytes from ring start to slot 0       */
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t head;   /* consumer owned      */
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t tail;   /* producer owned      */
    perl_krt_ring_bridge_t bridge;
} perl_krt_spsc_t;

static inline uint32_t
perl_krt_ring_round_slot(uint32_t slot_size)
{
    return (slot_size + 7u) & ~7u;
}

static inline size_t
perl_krt_spsc_bytes(uint32_t cap, uint32_t slot_size)
{
    return sizeof(perl_krt_spsc_t)
         + (size_t)cap * perl_krt_ring_round_slot(slot_size);
}

static inline perl_krt_spsc_t *
perl_krt_spsc_init_flags(void *mem, size_t mem_len, uint32_t cap,
                         uint32_t slot_size, uint32_t flags)
{
    perl_krt_spsc_t *r = (perl_krt_spsc_t *)mem;
    if (!mem || cap == 0 || (cap & (cap - 1)) || cap > (1u << 31)
        || slot_size == 0 || mem_len < perl_krt_spsc_bytes(cap, slot_size))
        return NULL;
    memset(r, 0, sizeof *r);
    r->cap       = cap;
    r->mask      = cap - 1;
    r->slot_size = perl_krt_ring_round_slot(slot_size);
    r->data_off  = (uint32_t)sizeof(perl_krt_spsc_t);
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->bridge.pub_seq, 0);
    atomic_init(&r->bridge.sleepers, 0);
    r->bridge.flags = flags;
    atomic_init(&r->magic, 0);
    perl_krt_store_release(&r->magic, PERL_KRT_RING_MAGIC_SPSC);
    return r;
}

static inline perl_krt_spsc_t *
perl_krt_spsc_init(void *mem, size_t mem_len, uint32_t cap, uint32_t slot_size)
{
    return perl_krt_spsc_init_flags(mem, mem_len, cap, slot_size, 0);
}

/* Attach to a ring another process initialised in shared memory. */
static inline perl_krt_spsc_t *
perl_krt_spsc_attach(void *mem)
{
    perl_krt_spsc_t *r = (perl_krt_spsc_t *)mem;
    return (r && perl_krt_load_acquire(&r->magic) == PERL_KRT_RING_MAGIC_SPSC)
           ? r : NULL;
}

static inline void *
perl_krt_spsc_slot(perl_krt_spsc_t *r, uint32_t pos)
{
    return (char *)r + r->data_off + (size_t)(pos & r->mask) * r->slot_size;
}

static inline int
perl_krt_spsc_push(perl_krt_spsc_t *r, const void *item)
{
    uint32_t tail = perl_krt_load_relaxed(&r->tail);
    uint32_t head = perl_krt_load_acquire(&r->head);
    if (tail - head >= r->cap)
        return -EAGAIN;
    memcpy(perl_krt_spsc_slot(r, tail), item, r->slot_size);
    perl_krt_store_release(&r->tail, tail + 1);
    perl_krt_bridge_publish(&r->bridge);
    return 0;
}

static inline int
perl_krt_spsc_pop(perl_krt_spsc_t *r, void *out)
{
    uint32_t head = perl_krt_load_relaxed(&r->head);
    uint32_t tail = perl_krt_load_acquire(&r->tail);
    if (head == tail)
        return -EAGAIN;
    memcpy(out, perl_krt_spsc_slot(r, head), r->slot_size);
    perl_krt_store_release(&r->head, head + 1);
    return 0;
}

static inline uint32_t
perl_krt_spsc_count(perl_krt_spsc_t *r)
{
    return perl_krt_load_acquire(&r->tail) - perl_krt_load_acquire(&r->head);
}

/* ---- MPSC --------------------------------------------------------------- */

typedef struct perl_krt_mpsc_slot_hdr {
    perl_krt_au32_t seq;       /* == pos: free; == pos+1: published        */
    uint32_t        pad;
} perl_krt_mpsc_slot_hdr_t;

typedef struct perl_krt_mpsc {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t magic;
    uint32_t cap;
    uint32_t mask;
    uint32_t slot_size;           /* header + data, rounded                 */
    uint32_t data_size;           /* user bytes per slot                    */
    uint32_t data_off;
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t head;   /* consumer owned      */
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t tail;   /* claimed by producers */
    perl_krt_ring_bridge_t bridge;
} perl_krt_mpsc_t;

static inline size_t
perl_krt_mpsc_bytes(uint32_t cap, uint32_t data_size)
{
    return sizeof(perl_krt_mpsc_t)
         + (size_t)cap * perl_krt_ring_round_slot(
                (uint32_t)sizeof(perl_krt_mpsc_slot_hdr_t) + data_size);
}

static inline perl_krt_mpsc_slot_hdr_t *
perl_krt_mpsc_slot(perl_krt_mpsc_t *r, uint32_t pos)
{
    return (perl_krt_mpsc_slot_hdr_t *)
        ((char *)r + r->data_off + (size_t)(pos & r->mask) * r->slot_size);
}

static inline void *
perl_krt_mpsc_slot_data(perl_krt_mpsc_slot_hdr_t *s)
{
    return (char *)s + sizeof(perl_krt_mpsc_slot_hdr_t);
}

static inline perl_krt_mpsc_t *
perl_krt_mpsc_init_flags(void *mem, size_t mem_len, uint32_t cap,
                         uint32_t data_size, uint32_t flags)
{
    perl_krt_mpsc_t *r = (perl_krt_mpsc_t *)mem;
    uint32_t i;
    if (!mem || cap == 0 || (cap & (cap - 1)) || cap > (1u << 30)
        || data_size == 0 || mem_len < perl_krt_mpsc_bytes(cap, data_size))
        return NULL;
    memset(r, 0, sizeof *r);
    r->cap       = cap;
    r->mask      = cap - 1;
    r->data_size = data_size;
    r->slot_size = perl_krt_ring_round_slot(
                       (uint32_t)sizeof(perl_krt_mpsc_slot_hdr_t) + data_size);
    r->data_off  = (uint32_t)sizeof(perl_krt_mpsc_t);
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->bridge.pub_seq, 0);
    atomic_init(&r->bridge.sleepers, 0);
    r->bridge.flags = flags;
    for (i = 0; i < cap; i++)
        atomic_init(&perl_krt_mpsc_slot(r, i)->seq, i);
    atomic_init(&r->magic, 0);
    perl_krt_store_release(&r->magic, PERL_KRT_RING_MAGIC_MPSC);
    return r;
}

static inline perl_krt_mpsc_t *
perl_krt_mpsc_init(void *mem, size_t mem_len, uint32_t cap, uint32_t data_size)
{
    return perl_krt_mpsc_init_flags(mem, mem_len, cap, data_size, 0);
}

static inline perl_krt_mpsc_t *
perl_krt_mpsc_attach(void *mem)
{
    perl_krt_mpsc_t *r = (perl_krt_mpsc_t *)mem;
    return (r && perl_krt_load_acquire(&r->magic) == PERL_KRT_RING_MAGIC_MPSC)
           ? r : NULL;
}

/* Step 1: claim a slot.  On success *pos_out is the claim position and
 * *data_out points at data_size writable bytes.  -EAGAIN when full.       */
static inline int
perl_krt_mpsc_claim(perl_krt_mpsc_t *r, uint32_t *pos_out, void **data_out)
{
    uint32_t pos = perl_krt_load_relaxed(&r->tail);
    for (;;) {
        perl_krt_mpsc_slot_hdr_t *s = perl_krt_mpsc_slot(r, pos);
        uint32_t seq = perl_krt_load_acquire(&s->seq);
        int32_t  diff = (int32_t)(seq - pos);
        if (diff == 0) {
            if (perl_krt_cas_weak_relaxed(&r->tail, &pos, pos + 1)) {
                *pos_out  = pos;
                *data_out = perl_krt_mpsc_slot_data(s);
                return 0;
            }
            /* pos refreshed by the CAS */
        }
        else if (diff < 0) {
            return -EAGAIN;      /* slot still held by the consumer: full */
        }
        else {
            pos = perl_krt_load_relaxed(&r->tail);
            perl_krt_cpu_relax();
        }
    }
}

/* Step 2: publish a claimed slot. */
static inline void
perl_krt_mpsc_publish(perl_krt_mpsc_t *r, uint32_t pos)
{
    perl_krt_store_release(&perl_krt_mpsc_slot(r, pos)->seq, pos + 1);
    perl_krt_bridge_publish(&r->bridge);
}

static inline int
perl_krt_mpsc_push(perl_krt_mpsc_t *r, const void *item, uint32_t len)
{
    uint32_t pos;
    void *d;
    int rc;
    if (len > r->data_size)
        return -EMSGSIZE;
    rc = perl_krt_mpsc_claim(r, &pos, &d);
    if (rc)
        return rc;
    memcpy(d, item, len);
    perl_krt_mpsc_publish(r, pos);
    return 0;
}

/* Consumer: copy out the oldest published slot.  -EAGAIN when empty.      */
static inline int
perl_krt_mpsc_pop(perl_krt_mpsc_t *r, void *out, uint32_t len)
{
    uint32_t head = perl_krt_load_relaxed(&r->head);
    perl_krt_mpsc_slot_hdr_t *s = perl_krt_mpsc_slot(r, head);
    uint32_t seq = perl_krt_load_acquire(&s->seq);
    if ((int32_t)(seq - (head + 1)) != 0)
        return -EAGAIN;
    memcpy(out, perl_krt_mpsc_slot_data(s), len < r->data_size ? len : r->data_size);
    perl_krt_store_release(&s->seq, head + r->cap);
    perl_krt_store_relaxed(&r->head, head + 1);
    return 0;
}

static inline uint32_t
perl_krt_mpsc_count(perl_krt_mpsc_t *r)
{
    return perl_krt_load_acquire(&r->tail) - perl_krt_load_acquire(&r->head);
}

/* ---- consumer parking --------------------------------------------------- */

/* Sleep until a producer publishes after `expected` (as read from the
 * bridge's pub_seq before the ring was found empty) or timeout_ns.        */
static inline void
perl_krt_bridge_wait(perl_krt_ring_bridge_t *b, uint32_t expected,
                     uint64_t timeout_ns)
{
    perl_krt_store_seq_cst(&b->sleepers, 1);
    if (perl_krt_load_seq_cst(&b->pub_seq) == expected)
        perl_krt_wait_u32_flags((volatile uint32_t *)&b->pub_seq, expected,
                                timeout_ns, perl_krt_bridge_wait_flags(b));
    perl_krt_store_seq_cst(&b->sleepers, 0);
}

#define perl_krt_spsc_seq(r)  perl_krt_load_acquire(&(r)->bridge.pub_seq)
#define perl_krt_mpsc_seq(r)  perl_krt_load_acquire(&(r)->bridge.pub_seq)
#define perl_krt_spsc_wait(r, expected, ns) \
    perl_krt_bridge_wait(&(r)->bridge, (expected), (ns))
#define perl_krt_mpsc_wait(r, expected, ns) \
    perl_krt_bridge_wait(&(r)->bridge, (expected), (ns))

#endif /* PERL_KRT_RING_H */

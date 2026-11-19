/*    krt_atomic.h
 *
 *    C11 atomics with the memory orders the krt rings need, plus the
 *    architecture-specific spin hints.  On arm64 the compiler emits LSE
 *    (LDADDAL/CASAL) when built with -march=armv8.1-a or later (Apple
 *    Silicon always has it); on ppc64le it emits ldarx/stdcx. loops; on
 *    amd64 lock xadd / cmpxchg.  No inline assembly is needed.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_ATOMIC_H
#define PERL_KRT_ATOMIC_H

#include <stdatomic.h>
#include <stdint.h>

typedef _Atomic uint32_t perl_krt_au32_t;
typedef _Atomic uint64_t perl_krt_au64_t;
typedef _Atomic(void *)  perl_krt_aptr_t;

#define perl_krt_load_relaxed(p)   atomic_load_explicit((p), memory_order_relaxed)
#define perl_krt_load_acquire(p)   atomic_load_explicit((p), memory_order_acquire)
#define perl_krt_store_relaxed(p, v) \
    atomic_store_explicit((p), (v), memory_order_relaxed)
#define perl_krt_store_release(p, v) \
    atomic_store_explicit((p), (v), memory_order_release)
#define perl_krt_fetch_add_relaxed(p, v) \
    atomic_fetch_add_explicit((p), (v), memory_order_relaxed)
#define perl_krt_fetch_add_acq_rel(p, v) \
    atomic_fetch_add_explicit((p), (v), memory_order_acq_rel)
#define perl_krt_fetch_add_release(p, v) \
    atomic_fetch_add_explicit((p), (v), memory_order_release)
#define perl_krt_fetch_sub_acq_rel(p, v) \
    atomic_fetch_sub_explicit((p), (v), memory_order_acq_rel)
#define perl_krt_cas_weak_relaxed(p, expected, desired) \
    atomic_compare_exchange_weak_explicit((p), (expected), (desired), \
        memory_order_relaxed, memory_order_relaxed)
#define perl_krt_cas_strong_acq_rel(p, expected, desired) \
    atomic_compare_exchange_strong_explicit((p), (expected), (desired), \
        memory_order_acq_rel, memory_order_acquire)
#define perl_krt_exchange_acq_rel(p, v) \
    atomic_exchange_explicit((p), (v), memory_order_acq_rel)
#define perl_krt_load_seq_cst(p)   atomic_load_explicit((p), memory_order_seq_cst)
#define perl_krt_store_seq_cst(p, v) \
    atomic_store_explicit((p), (v), memory_order_seq_cst)
#define perl_krt_fetch_add_seq_cst(p, v) \
    atomic_fetch_add_explicit((p), (v), memory_order_seq_cst)

/* The TLS base register, or NULL when unknown; used for rseq lookups.  */
static inline void *perl_krt_thread_pointer(void)
{
#if defined(__powerpc64__) || defined(__ppc64__)
    register void *tp __asm__("r13");
    __asm__("" : "=r"(tp));
    return tp;
#elif defined(__aarch64__) || defined(__arm64__)
    void *tp;
    __asm__("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
#elif defined(__x86_64__)
    void *tp;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    return tp;
#else
    return (void *)0;
#endif
}

static inline void perl_krt_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__powerpc64__) || defined(__ppc64__)
    /* lower SMT priority while spinning, then restore */
    __asm__ __volatile__("or 1,1,1\n\tor 2,2,2" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

#endif /* PERL_KRT_ATOMIC_H */

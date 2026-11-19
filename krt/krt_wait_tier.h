/*    krt_wait_tier.h
 *
 *    The CPU-dependent half of perl_krt_wait_line, shared by the Linux and
 *    Darwin backends: which wait instruction the CPU offers is a property
 *    of the CPU, not of the kernel.  Internal, not installed.  Included only
 *    inside the backend's OS gate.
 *
 *    Ticks are the CPU's own counter: rdtsc (calibrated), cntvct_el0 (its
 *    frequency is read from cntfrq_el0), mfspr 268 (calibrated), or the
 *    monotonic clock where none of those apply.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_WAIT_TIER_H
#define PERL_KRT_WAIT_TIER_H

#include <stdint.h>
#include "krt.h"
#include "krt_atomic.h"
#include "krt_wait.h"

#if defined(__x86_64__)
#  include <cpuid.h>
#  include <immintrin.h>
#endif

enum krt_tier {
    KRT_TIER_UNSET = 0,
    KRT_TIER_UMWAIT,
    KRT_TIER_PAUSE,
    KRT_TIER_WFE,
    KRT_TIER_PPC_NAP,
    KRT_TIER_SPIN
};

static perl_krt_au32_t krt_tier_cache;   /* 0 = not yet detected          */
static perl_krt_au64_t krt_tb_hz_cache;  /* ticks per second, 0 = unset   */

static inline uint64_t
krt_tb_now(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__) || defined(__arm64__)
    uint64_t t;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#elif defined(__powerpc64__) || defined(__ppc64__)
    uint64_t t;
    __asm__ __volatile__("mfspr %0, 268" : "=r"(t));
    return t;
#else
    return perl_krt_now_ns();
#endif
}

static inline uint64_t
krt_tb_hz(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#elif defined(__x86_64__) || defined(__powerpc64__) || defined(__ppc64__)
    uint64_t hz = perl_krt_load_relaxed(&krt_tb_hz_cache);
    uint64_t n0, n1, t0, t1;
    if (hz)
        return hz;
    n0 = perl_krt_now_ns();
    t0 = krt_tb_now();
    do {
        n1 = perl_krt_now_ns();
    } while (n1 - n0 < 2000000ull);
    t1 = krt_tb_now();
    hz = (t1 - t0) * 1000000000ull / (n1 - n0);
    if (hz == 0)
        hz = 1;
    perl_krt_store_relaxed(&krt_tb_hz_cache, hz);   /* any racer's value is good */
    return hz;
#else
    return 1000000000ull;
#endif
}

/* ns -> ticks without overflow for budgets up to one second. */
static inline uint64_t
krt_ns_to_tb(uint64_t ns, uint64_t hz)
{
    if (ns > 1000000000ull)
        ns = 1000000000ull;
    return (ns / 1000000000ull) * hz + (ns % 1000000000ull) * hz / 1000000000ull;
}

static inline int
krt_tier(void)
{
    uint32_t t = perl_krt_load_relaxed(&krt_tier_cache);
    if (t)
        return (int)t;
#if defined(__x86_64__)
    {
        unsigned a = 0, b = 0, c = 0, d = 0;
        /* leaf 7, subleaf 0, ECX bit 5: WAITPKG (umonitor / umwait) */
        if (__get_cpuid_count(7, 0, &a, &b, &c, &d) && (c & (1u << 5)))
            t = KRT_TIER_UMWAIT;
        else
            t = KRT_TIER_PAUSE;
    }
#elif defined(__i386__)
    t = KRT_TIER_PAUSE;
#elif defined(__aarch64__) || defined(__arm64__)
    t = KRT_TIER_WFE;
#elif defined(__powerpc64__) || defined(__ppc64__)
    t = KRT_TIER_PPC_NAP;
#else
    t = KRT_TIER_SPIN;
#endif
    perl_krt_store_relaxed(&krt_tier_cache, t);
    return (int)t;
}

static inline const char *
krt_tier_name(int t)
{
    switch (t) {
    case KRT_TIER_UMWAIT:  return "umwait";
    case KRT_TIER_PAUSE:   return "pause";
    case KRT_TIER_WFE:     return "wfe";
    case KRT_TIER_PPC_NAP: return "ppc-nap";
    default:               return "spin";
    }
}

/* Each loop: read the word (acquire), test the budget, then wait.  The
 * acquire load pairs with the poster's release (or seq_cst) bump.       */

#if defined(__x86_64__)
__attribute__((target("waitpkg")))
static inline perl_krt_wait_reason_t
krt_wait_umwait(const uint32_t *addr, uint32_t expected, uint64_t end)
{
    for (;;) {
        if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
            return PERL_KRT_WAIT_CHANGED;
        _umonitor((void *)addr);
        /* re-check after arming: a store between the load and the
         * monitor would otherwise be missed                              */
        if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
            return PERL_KRT_WAIT_CHANGED;
        if (krt_tb_now() >= end)
            return PERL_KRT_WAIT_BUDGET;
        /* the kernel caps one umwait at umwait_control max_time, so the
         * loop re-enters until the deadline                              */
        _umwait(1, end);                /* control 1: C0.1 */
    }
}
#endif

static inline perl_krt_wait_reason_t
krt_wait_pause(const uint32_t *addr, uint32_t expected, uint64_t end)
{
    for (;;) {
        if (__atomic_load_n(addr, __ATOMIC_ACQUIRE) != expected)
            return PERL_KRT_WAIT_CHANGED;
        if (krt_tb_now() >= end)
            return PERL_KRT_WAIT_BUDGET;
        perl_krt_cpu_relax();
    }
}

#if defined(__aarch64__) || defined(__arm64__)
static inline perl_krt_wait_reason_t
krt_wait_wfe(const uint32_t *addr, uint32_t expected, uint64_t end)
{
    for (;;) {
        uint32_t v;
        /* ldaxr arms the exclusive monitor on the word; a store by another
         * CPU clears it and sets the event register, so wfe returns      */
        __asm__ __volatile__("ldaxr %w0, [%1]"
                             : "=&r"(v) : "r"(addr) : "memory");
        if (v != expected)
            return PERL_KRT_WAIT_CHANGED;
        if (krt_tb_now() >= end)
            return PERL_KRT_WAIT_BUDGET;
        __asm__ __volatile__("wfe" ::: "memory");
    }
}
#endif

#if defined(__powerpc64__) || defined(__ppc64__)
static inline perl_krt_wait_reason_t
krt_wait_ppc_nap(const uint32_t *addr, uint32_t expected, uint64_t end)
{
    for (;;) {
        uint32_t v;
        __asm__ __volatile__("or 31,31,31" ::: "memory");   /* very low prio */
        v = __atomic_load_n(addr, __ATOMIC_ACQUIRE);
        if (v != expected) {
            __asm__ __volatile__("or 2,2,2" ::: "memory");  /* medium prio */
            return PERL_KRT_WAIT_CHANGED;
        }
        if (krt_tb_now() >= end) {
            __asm__ __volatile__("or 2,2,2" ::: "memory");
            return PERL_KRT_WAIT_BUDGET;
        }
    }
}
#endif

static inline perl_krt_wait_reason_t
krt_wait_line_impl(const volatile uint32_t *addr, uint32_t expected,
                   uint64_t budget_ns)
{
    const uint32_t *a = (const uint32_t *)addr;
    uint64_t hz = krt_tb_hz();
    uint64_t end = krt_tb_now() + krt_ns_to_tb(budget_ns, hz);
    switch (krt_tier()) {
#if defined(__x86_64__)
    case KRT_TIER_UMWAIT:
        return krt_wait_umwait(a, expected, end);
#endif
#if defined(__aarch64__) || defined(__arm64__)
    case KRT_TIER_WFE:
        return krt_wait_wfe(a, expected, end);
#endif
#if defined(__powerpc64__) || defined(__ppc64__)
    case KRT_TIER_PPC_NAP:
        return krt_wait_ppc_nap(a, expected, end);
#endif
    default:
        return krt_wait_pause(a, expected, end);
    }
}

#endif /* PERL_KRT_WAIT_TIER_H */

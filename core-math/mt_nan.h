/*
 * Canonical quiet NaN enforcement for IEEE-754 binary64.
 *
 * Different architectures produce distinct NaN bit patterns (e.g. POWER
 * signs 0*Inf differently from x86_64 and AArch64, and payloads can vary).
 * Normalising every NaN at op and conversion boundaries to the single
 * positive quiet NaN 0x7ff8000000000000 ensures bit-identical results
 * across hosts for comparisons, stringification, and pack/unpack.
 */

#ifndef MT_CORE_MATH_NAN_H
#define MT_CORE_MATH_NAN_H

#include <stdint.h>
#include <string.h>

#define MT_CANON_QNAN_U64 UINT64_C(0x7ff8000000000000)

static inline double
mt_canon_double(double x)
{
    if (x != x) {
        uint64_t u = MT_CANON_QNAN_U64;
        double qnan;
        memcpy(&qnan, &u, sizeof(qnan));
        return qnan;
    }
    return x;
}

#if defined(NVSIZE) && !defined(USE_LONG_DOUBLE) && !defined(USE_QUADMATH)
#  define MT_NV_IS_BINARY64 (NVSIZE == 8)
#elif !defined(USE_LONG_DOUBLE) && !defined(USE_QUADMATH)
#  define MT_NV_IS_BINARY64 (sizeof(NV) == 8)
#else
#  define MT_NV_IS_BINARY64 0
#endif

#if defined(NV) || defined(PERL_CORE)
static inline NV
mt_canon_nv(NV x)
{
#if MT_NV_IS_BINARY64
    if (x != x) {
        uint64_t u = MT_CANON_QNAN_U64;
        double qnan;
        memcpy(&qnan, &u, sizeof(qnan));
        return (NV)qnan;
    }
#endif
    return x;
}
#endif

#endif /* MT_CORE_MATH_NAN_H */

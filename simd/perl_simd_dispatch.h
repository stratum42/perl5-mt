/*
 * perl5-mt: runtime SIMD dispatch across host CPU ISA tiers.
 *
 * PL_simd_level is set once, from perl_construct, by Perl_simd_detect()
 * (simd/perl_simd.c).  Every kernel has a scalar twin, which is the reference
 * and is byte-for-byte the upstream behaviour, and vector bodies that are
 * selected by a switch on the level.  The vector bodies are compiled with
 * GCC/Clang vector extensions: a 16-byte body runs on the baseline vector
 * unit of each host (SSE2, VSX, NEON) and a 32-byte body is compiled for
 * AVX2 with __attribute__((target("avx2"))) on amd64.
 *
 * Backward compatibility:
 *   PL_simd_level ALWAYS holds one of the baseline values (0..4: SCALAR,
 *   SSE2, AVX2, VSX, NEON) so existing switch statements and name tables
 *   remain 100% compatible.  Extended ISA tiers (AVX-512, AVX2-VNNI,
 *   AVX-512-VNNI, VSX2, VSX3, s390x VX, s390x NNPA) are queried through
 *   Perl_simd_extended_level() and Perl_simd_caps().
 *
 * Gates:
 *   PERL_FORCE_SCALAR=1 in the environment (read by Perl_simd_detect) selects
 *       the scalar level at run time;
 *   PERL_SIMD_LEVEL=scalar|narrow|<tier> (environment, test knob) caps the
 *       level: narrow stops at the 16-byte body, so AVX2/AVX-512 hosts can
 *       run the SSE2 path;
 *   -DPERL_FORCE_SCALAR compiles PERL_SIMD_DISPATCH(name, ...) straight to
 *       the scalar twin, with no vector code in the object at all.
 */

#ifndef PERL_SIMD_DISPATCH_H
#define PERL_SIMD_DISPATCH_H

#include <stdint.h>

/* Baseline SIMD tiers (0..4): stored in PL_simd_level */
#define PERL_SIMD_SCALAR        0
#define PERL_SIMD_SSE2          1
#define PERL_SIMD_AVX2          2
#define PERL_SIMD_VSX           3
#define PERL_SIMD_NEON          4

/* Extended CPU ISA tiers (5..11): returned by Perl_simd_extended_level() */
#define PERL_SIMD_AVX512        5   /* x86-64 AVX-512 F/BW/VL */
#define PERL_SIMD_AVX2_VNNI     6   /* x86-64 AVX2 + VNNI (Intel/AMD Zen 4-5) */
#define PERL_SIMD_AVX512_VNNI   7   /* x86-64 AVX-512 F/BW/VL + VNNI */
#define PERL_SIMD_VSX2          8   /* POWER8 VSX2 (ISA 2.07) */
#define PERL_SIMD_VSX3          9   /* POWER9/10 VSX3 (ISA 3.0+) */
#define PERL_SIMD_S390X_VX     10   /* IBM z13-z16 Vector Facility */
#define PERL_SIMD_S390X_NNPA   11   /* IBM z16 / Telum II NNPA Facility */
#define PERL_SIMD_LEVEL_COUNT  12

typedef int perl_simd_level_t;

/* Fine-grained CPU ISA capability flags returned by Perl_simd_caps() */
#define PERL_SIMD_CAP_SSE2          (UINT64_C(1) << 0)
#define PERL_SIMD_CAP_AVX2          (UINT64_C(1) << 1)
#define PERL_SIMD_CAP_AVX512F       (UINT64_C(1) << 2)
#define PERL_SIMD_CAP_AVX512BW      (UINT64_C(1) << 3)
#define PERL_SIMD_CAP_AVX512VL      (UINT64_C(1) << 4)
#define PERL_SIMD_CAP_AVX_VNNI      (UINT64_C(1) << 5)
#define PERL_SIMD_CAP_AVX512_VNNI   (UINT64_C(1) << 6)
#define PERL_SIMD_CAP_NEON          (UINT64_C(1) << 7)
#define PERL_SIMD_CAP_NEON_DOTPROD  (UINT64_C(1) << 8)
#define PERL_SIMD_CAP_NEON_I8MM     (UINT64_C(1) << 9)
#define PERL_SIMD_CAP_APPLE_AMX     (UINT64_C(1) << 10)
#define PERL_SIMD_CAP_VSX           (UINT64_C(1) << 11)
#define PERL_SIMD_CAP_VSX2          (UINT64_C(1) << 12)
#define PERL_SIMD_CAP_VSX3          (UINT64_C(1) << 13)
#define PERL_SIMD_CAP_S390X_VX      (UINT64_C(1) << 14)
#define PERL_SIMD_CAP_S390X_NNPA    (UINT64_C(1) << 15)

/* Extended runtime detection query API (implemented in simd/perl_simd.c) */
perl_simd_level_t Perl_simd_extended_level(void);
uint64_t          Perl_simd_caps(void);
const char       *Perl_simd_level_name(perl_simd_level_t level);
perl_simd_level_t Perl_simd_baseline_level(perl_simd_level_t ext_level);

#ifdef PERL_SIMD_STANDALONE
perl_simd_level_t Perl_simd_detect_standalone(void);
void              Perl_simd_reset_for_test(void);
#endif

/* Hooks take the SIMD path only for strings at least this long; shorter ones
 * keep the existing code exactly. */
#define PERL_SIMD_MIN_LEN 64

#if defined(__GNUC__) || defined(__clang__)
#  define PERL_SIMD_VEC
#  if defined(__x86_64__)
#    define PERL_SIMD_HAVE_W32
#  endif
#endif

/* A hook takes its SIMD path only when PERL_SIMD_ON() is true: never under
 * -DPERL_FORCE_SCALAR, and at run time only when the level is not scalar.  In
 * scalar mode a hook therefore runs the upstream code unchanged, so the
 * scalar child in the parity test is upstream exactly. */
/* The kernels assume the ASCII character set (invariant bytes are below 0x80),
 * so an EBCDIC build never takes them. */
#if defined(PERL_FORCE_SCALAR) || defined(EBCDIC)
#  define PERL_SIMD_ON()                0
#else
#  define PERL_SIMD_ON()                (PL_simd_level != PERL_SIMD_SCALAR)
#endif

#ifdef PERL_FORCE_SCALAR
#  define PERL_SIMD_DISPATCH(name, ...) name##_scalar(__VA_ARGS__)
#else
#  define PERL_SIMD_DISPATCH(name, ...) name(__VA_ARGS__)
#endif

#ifndef PERL_SIMD_STANDALONE
/* Endian-neutral test for "some byte in this vector has its high bit set".
 * PERL_UINTMAX_T words are read with memcpy, so no alignment is assumed. */
#define PERL_SIMD_HIGH_WORD_MASK ((~ (PERL_UINTMAX_T) 0 / 0xFF) * 0x80)

static inline bool
perl_simd_bytes_have_high(const void *v, size_t bytes)
{
    PERL_UINTMAX_T w;
    size_t i;

    for (i = 0; i < bytes; i += sizeof w) {
        memcpy(&w, (const U8 *) v + i, sizeof w);
        if (w & PERL_SIMD_HIGH_WORD_MASK) {
            return TRUE;
        }
    }

    return FALSE;
}
#endif /* !PERL_SIMD_STANDALONE */

#endif /* PERL_SIMD_DISPATCH_H */

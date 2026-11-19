/*
 * Unit tests for expanded CPU ISA tier detection in simd/perl_simd.c.
 */

#include "accel_test.h"
#include "perl_simd_dispatch.h"

#include <stdlib.h>

int
main(void)
{
    perl_simd_level_t ext, base;
    uint64_t caps;

    /* 1. Default hardware detection (clear env overrides first) */
    unsetenv("PERL_FORCE_SCALAR");
    unsetenv("PERL_SIMD_LEVEL");
    Perl_simd_reset_for_test();

    ext = Perl_simd_detect_standalone();
    base = Perl_simd_baseline_level(ext);
    caps = Perl_simd_caps();

    ok(ext >= PERL_SIMD_SCALAR && ext < PERL_SIMD_LEVEL_COUNT,
       "extended SIMD level in valid range (%d: %s)", ext, Perl_simd_level_name(ext));
    ok(base >= PERL_SIMD_SCALAR && base <= PERL_SIMD_NEON,
       "baseline SIMD level stays in legacy 0..4 range (%d: %s)",
       base, Perl_simd_level_name(base));

#if defined(__aarch64__)
    is_int(ext, PERL_SIMD_NEON, "arm64 detects NEON extended level");
    is_int(base, PERL_SIMD_NEON, "arm64 baseline level is NEON");
    ok((caps & PERL_SIMD_CAP_NEON) != 0, "arm64 sets PERL_SIMD_CAP_NEON");
#elif defined(__x86_64__)
    ok(base == PERL_SIMD_SSE2 || base == PERL_SIMD_AVX2,
       "x86-64 baseline is SSE2 or AVX2");
    ok((caps & PERL_SIMD_CAP_SSE2) != 0, "x86-64 sets PERL_SIMD_CAP_SSE2");
#else
    (void)caps;
#endif

    /* 2. Verify baseline mapping for all extended tiers */
    is_int(Perl_simd_baseline_level(PERL_SIMD_SCALAR),      PERL_SIMD_SCALAR, "baseline(SCALAR)");
    is_int(Perl_simd_baseline_level(PERL_SIMD_SSE2),        PERL_SIMD_SSE2,   "baseline(SSE2)");
    is_int(Perl_simd_baseline_level(PERL_SIMD_AVX2),        PERL_SIMD_AVX2,   "baseline(AVX2)");
    is_int(Perl_simd_baseline_level(PERL_SIMD_AVX512),      PERL_SIMD_AVX2,   "baseline(AVX512) -> AVX2");
    is_int(Perl_simd_baseline_level(PERL_SIMD_AVX2_VNNI),   PERL_SIMD_AVX2,   "baseline(AVX2_VNNI) -> AVX2");
    is_int(Perl_simd_baseline_level(PERL_SIMD_AVX512_VNNI), PERL_SIMD_AVX2,   "baseline(AVX512_VNNI) -> AVX2");
    is_int(Perl_simd_baseline_level(PERL_SIMD_VSX),         PERL_SIMD_VSX,    "baseline(VSX)");
    is_int(Perl_simd_baseline_level(PERL_SIMD_VSX2),        PERL_SIMD_VSX,    "baseline(VSX2) -> VSX");
    is_int(Perl_simd_baseline_level(PERL_SIMD_VSX3),        PERL_SIMD_VSX,    "baseline(VSX3) -> VSX");
    is_int(Perl_simd_baseline_level(PERL_SIMD_NEON),        PERL_SIMD_NEON,   "baseline(NEON)");
    is_int(Perl_simd_baseline_level(PERL_SIMD_S390X_VX),    PERL_SIMD_SCALAR, "baseline(S390X_VX) -> SCALAR");
    is_int(Perl_simd_baseline_level(PERL_SIMD_S390X_NNPA),  PERL_SIMD_SCALAR, "baseline(S390X_NNPA) -> SCALAR");

    /* 3. Verify names for all tiers */
    is_str(Perl_simd_level_name(PERL_SIMD_AVX512),      "avx512",      "name(AVX512)");
    is_str(Perl_simd_level_name(PERL_SIMD_AVX2_VNNI),   "avx2_vnni",   "name(AVX2_VNNI)");
    is_str(Perl_simd_level_name(PERL_SIMD_AVX512_VNNI), "avx512_vnni", "name(AVX512_VNNI)");
    is_str(Perl_simd_level_name(PERL_SIMD_VSX2),        "vsx2",        "name(VSX2)");
    is_str(Perl_simd_level_name(PERL_SIMD_VSX3),        "vsx3",        "name(VSX3)");
    is_str(Perl_simd_level_name(PERL_SIMD_S390X_VX),    "s390x_vx",    "name(S390X_VX)");
    is_str(Perl_simd_level_name(PERL_SIMD_S390X_NNPA),  "s390x_nnpa",  "name(S390X_NNPA)");

    /* 4. PERL_FORCE_SCALAR=1 override */
    setenv("PERL_FORCE_SCALAR", "1", 1);
    Perl_simd_reset_for_test();
    is_int(Perl_simd_detect_standalone(), PERL_SIMD_SCALAR, "PERL_FORCE_SCALAR=1 forces scalar");
    is_int(Perl_simd_caps(), 0, "PERL_FORCE_SCALAR=1 clears caps");
    unsetenv("PERL_FORCE_SCALAR");

    /* 5. PERL_SIMD_LEVEL=scalar override */
    setenv("PERL_SIMD_LEVEL", "scalar", 1);
    Perl_simd_reset_for_test();
    is_int(Perl_simd_detect_standalone(), PERL_SIMD_SCALAR, "PERL_SIMD_LEVEL=scalar forces scalar");
    unsetenv("PERL_SIMD_LEVEL");

    done_testing();
}

/*
 * perl5-mt: detection of host CPU SIMD tiers and extensible ISA dispatch.
 *
 * Perl_simd_detect() runs once, from perl_construct, and sets PL_simd_level
 * (baseline 0..4 for backward compatibility) as well as the extended tier
 * and capability bitmask queried via Perl_simd_extended_level() and
 * Perl_simd_caps().
 *
 * Can also be compiled standalone with -DPERL_SIMD_STANDALONE for unit tests
 * and the accel/ library.
 */

#ifndef PERL_SIMD_STANDALONE
#  include "../EXTERN.h"
#  include "../perl.h"
#else
#  include "perl_simd_dispatch.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#  include <cpuid.h>
#endif

#if defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

#if defined(__linux__) && (defined(__powerpc64__) || defined(__powerpc__) || \
                           defined(__aarch64__) || defined(__s390x__))
#  include <sys/auxv.h>
#endif

#if defined(__powerpc64__) || defined(__powerpc__)
#  if defined(__linux__)
#    ifndef PPC_FEATURE_HAS_VSX
#      define PPC_FEATURE_HAS_VSX     0x00000080UL
#    endif
#    ifndef PPC_FEATURE2_ARCH_2_07
#      define PPC_FEATURE2_ARCH_2_07  0x80000000UL
#    endif
#    ifndef PPC_FEATURE2_ARCH_3_00
#      define PPC_FEATURE2_ARCH_3_00  0x00800000UL
#    endif
#    ifndef PPC_FEATURE2_ARCH_3_1
#      define PPC_FEATURE2_ARCH_3_1   0x00040000UL
#    endif
#  elif defined(_AIX)
#    include <sys/systemcfg.h>
#  endif
#endif

#if defined(__s390x__) && defined(__linux__)
#  ifndef HWCAP_S390_VX
#    define HWCAP_S390_VX             (1UL << 11)
#  endif
#endif

#if defined(__aarch64__) && defined(__linux__)
#  ifndef HWCAP_ASIMDDP
#    define HWCAP_ASIMDDP             (1UL << 20)
#  endif
#  ifndef HWCAP2_I8MM
#    define HWCAP2_I8MM               (1UL << 13)
#  endif
#endif

static int               S_simd_detected = 0;
static perl_simd_level_t S_simd_ext_level = PERL_SIMD_SCALAR;
static uint64_t          S_simd_caps = 0;

static const char * const S_simd_ext_names[PERL_SIMD_LEVEL_COUNT] = {
    "scalar",
    "sse2",
    "avx2",
    "vsx",
    "neon",
    "avx512",
    "avx2_vnni",
    "avx512_vnni",
    "vsx2",
    "vsx3",
    "s390x_vx",
    "s390x_nnpa"
};

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static uint64_t
S_x86_xgetbv0(void)
{
    uint32_t eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | eax;
}
#endif

#if defined(__s390x__) && (defined(__GNUC__) || defined(__clang__))
/*
 * Query s390x facility bit via STFLE (Store Facility List Extended).
 * Facility bit 129: Vector Facility (z13+).
 * Facility bit 165: Neural Network Processing Assist (NNPA, z16 / Telum II).
 * Bits are numbered big-endian within each 64-bit doubleword:
 * word = bit / 64, mask = 1ULL << (63 - (bit % 64)).
 */
static int
S_s390x_has_facility(unsigned int bit)
{
    uint64_t fac[4] = { 0, 0, 0, 0 };
    register unsigned long r0 __asm__("0") = 3; /* store 4 doublewords (0..255) */

    if (bit >= 256) {
        return 0;
    }

    __asm__ __volatile__(
        ".insn s,0xb2b00000,%0\n"
        : "+Q"(fac), "+d"(r0)
        :
        : "cc", "memory"
    );

    return (fac[bit / 64] & (UINT64_C(1) << (63 - (bit % 64)))) != 0;
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
static int
S_darwin_sysctl_int(const char *name)
{
    int val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname(name, &val, &len, NULL, 0) == 0) {
        return val;
    }
    return 0;
}
#endif

/* Probe hardware ISA tier and populate capability bitmask */
static perl_simd_level_t
S_simd_hardware_probe(uint64_t *caps_out)
{
    uint64_t caps = 0;
    perl_simd_level_t level = PERL_SIMD_SCALAR;

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int max_leaf = __get_cpuid_max(0, NULL);

    /* SSE2 is mandatory on x86-64 */
    caps |= PERL_SIMD_CAP_SSE2;
    level = PERL_SIMD_SSE2;

    if (max_leaf >= 7) {
        int os_avx = 0;
        int os_avx512 = 0;

        __cpuid(1, eax, ebx, ecx, edx);
        /* Bit 27: OSXSAVE, Bit 28: AVX */
        if ((ecx & (1u << 27)) && (ecx & (1u << 28))) {
            uint64_t xcr0 = S_x86_xgetbv0();
            if ((xcr0 & 0x06ULL) == 0x06ULL) {
                os_avx = 1;
            }
            if ((xcr0 & 0xE6ULL) == 0xE6ULL) {
                os_avx512 = 1;
            }
        }

        if (os_avx) {
            unsigned int l7_eax = 0, l7_ebx = 0, l7_ecx = 0, l7_edx = 0;
            __cpuid_count(7, 0, l7_eax, l7_ebx, l7_ecx, l7_edx);

            /* EBX bit 5: AVX2 */
            if (l7_ebx & (1u << 5)) {
                caps |= PERL_SIMD_CAP_AVX2;
                level = PERL_SIMD_AVX2;
            }

            /* Leaf 7 subleaf 1: EAX bit 4 is AVX-VNNI (Intel Alder Lake+, AMD Zen 4/5) */
            if (l7_eax >= 1) {
                unsigned int s1_eax = 0, s1_ebx = 0, s1_ecx = 0, s1_edx = 0;
                __cpuid_count(7, 1, s1_eax, s1_ebx, s1_ecx, s1_edx);
                if (s1_eax & (1u << 4)) {
                    caps |= PERL_SIMD_CAP_AVX_VNNI;
                    if (caps & PERL_SIMD_CAP_AVX2) {
                        level = PERL_SIMD_AVX2_VNNI;
                    }
                }
            }

            if (os_avx512) {
                if (l7_ebx & (1u << 16)) caps |= PERL_SIMD_CAP_AVX512F;
                if (l7_ebx & (1u << 30)) caps |= PERL_SIMD_CAP_AVX512BW;
                if (l7_ebx & (1u << 31)) caps |= PERL_SIMD_CAP_AVX512VL;
                if (l7_ecx & (1u << 11)) caps |= PERL_SIMD_CAP_AVX512_VNNI;

                if ((caps & PERL_SIMD_CAP_AVX512F) &&
                    (caps & PERL_SIMD_CAP_AVX512BW) &&
                    (caps & PERL_SIMD_CAP_AVX512VL)) {
                    if (caps & PERL_SIMD_CAP_AVX512_VNNI) {
                        level = PERL_SIMD_AVX512_VNNI;
                    } else {
                        level = PERL_SIMD_AVX512;
                    }
                }
            }
        }
    }

#elif defined(__powerpc64__) || defined(__powerpc__)
#  if defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    unsigned long hwcap2 = getauxval(AT_HWCAP2);

    if (hwcap & PPC_FEATURE_HAS_VSX) {
        caps |= PERL_SIMD_CAP_VSX;
        level = PERL_SIMD_VSX;

        if (hwcap2 & PPC_FEATURE2_ARCH_2_07) {
            caps |= PERL_SIMD_CAP_VSX2;
            level = PERL_SIMD_VSX2;
        }
        if (hwcap2 & (PPC_FEATURE2_ARCH_3_00 | PPC_FEATURE2_ARCH_3_1)) {
            caps |= PERL_SIMD_CAP_VSX3;
            level = PERL_SIMD_VSX3;
        }
    }
#  elif defined(_AIX)
    if (__power_9_andup()) {
        caps |= PERL_SIMD_CAP_VSX | PERL_SIMD_CAP_VSX2 | PERL_SIMD_CAP_VSX3;
        level = PERL_SIMD_VSX3;
    } else if (__power_8_andup()) {
        caps |= PERL_SIMD_CAP_VSX | PERL_SIMD_CAP_VSX2;
        level = PERL_SIMD_VSX2;
    } else if (__power_7_andup()) {
        caps |= PERL_SIMD_CAP_VSX;
        level = PERL_SIMD_VSX;
    }
#  endif

#elif defined(__s390x__)
#  if defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_S390_VX) || S_s390x_has_facility(129)) {
        caps |= PERL_SIMD_CAP_S390X_VX;
        level = PERL_SIMD_S390X_VX;
        if (S_s390x_has_facility(165)) {
            caps |= PERL_SIMD_CAP_S390X_NNPA;
            level = PERL_SIMD_S390X_NNPA;
        }
    }
#  endif

#elif defined(__aarch64__)
    /* NEON is part of the arm64 baseline */
    caps |= PERL_SIMD_CAP_NEON;
    level = PERL_SIMD_NEON;

#  if defined(__APPLE__)
    if (S_darwin_sysctl_int("hw.optional.arm.FEAT_DotProd") > 0) {
        caps |= PERL_SIMD_CAP_NEON_DOTPROD;
    }
    if (S_darwin_sysctl_int("hw.optional.arm.FEAT_I8MM") > 0) {
        caps |= PERL_SIMD_CAP_NEON_I8MM;
    }
    if (S_darwin_sysctl_int("hw.optional.amx_version") > 0) {
        caps |= PERL_SIMD_CAP_APPLE_AMX;
    }
#  elif defined(__linux__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if (hwcap & HWCAP_ASIMDDP) {
        caps |= PERL_SIMD_CAP_NEON_DOTPROD;
    }
    if (hwcap2 & HWCAP2_I8MM) {
        caps |= PERL_SIMD_CAP_NEON_I8MM;
    }
#  endif

#endif

    if (caps_out) {
        *caps_out = caps;
    }
    return level;
}

perl_simd_level_t
Perl_simd_baseline_level(perl_simd_level_t ext_level)
{
    switch (ext_level) {
    case PERL_SIMD_SCALAR:
        return PERL_SIMD_SCALAR;
    case PERL_SIMD_SSE2:
        return PERL_SIMD_SSE2;
    case PERL_SIMD_AVX2:
    case PERL_SIMD_AVX512:
    case PERL_SIMD_AVX2_VNNI:
    case PERL_SIMD_AVX512_VNNI:
        return PERL_SIMD_AVX2;
    case PERL_SIMD_VSX:
    case PERL_SIMD_VSX2:
    case PERL_SIMD_VSX3:
        return PERL_SIMD_VSX;
    case PERL_SIMD_NEON:
        return PERL_SIMD_NEON;
    case PERL_SIMD_S390X_VX:
    case PERL_SIMD_S390X_NNPA:
    default:
        return PERL_SIMD_SCALAR;
    }
}

const char *
Perl_simd_level_name(perl_simd_level_t level)
{
    if (level < 0 || level >= PERL_SIMD_LEVEL_COUNT) {
        return "unknown";
    }
    return S_simd_ext_names[level];
}

static perl_simd_level_t
S_simd_detect_internal(void)
{
    const char *env;
    perl_simd_level_t level;
    uint64_t caps = 0;

    if (S_simd_detected) {
        return S_simd_ext_level;
    }
    S_simd_detected = 1;

    env = getenv("PERL_FORCE_SCALAR");
    if (env && *env && strcmp(env, "0") != 0) {
        S_simd_ext_level = PERL_SIMD_SCALAR;
        S_simd_caps = 0;
        return PERL_SIMD_SCALAR;
    }

    level = S_simd_hardware_probe(&caps);

    /*
     * PERL_SIMD_LEVEL=scalar forces scalar;
     * PERL_SIMD_LEVEL=narrow caps 32/64-byte x86 tiers at 16-byte SSE2;
     * explicit tier names allow capping down to a supported tier.
     */
    env = getenv("PERL_SIMD_LEVEL");
    if (env && *env) {
        if (strcmp(env, "scalar") == 0) {
            level = PERL_SIMD_SCALAR;
            caps = 0;
        } else if (strcmp(env, "narrow") == 0) {
            if (level == PERL_SIMD_AVX2 ||
                level == PERL_SIMD_AVX512 ||
                level == PERL_SIMD_AVX2_VNNI ||
                level == PERL_SIMD_AVX512_VNNI) {
                level = PERL_SIMD_SSE2;
            }
        } else if (strcmp(env, "sse2") == 0 && (caps & PERL_SIMD_CAP_SSE2)) {
            level = PERL_SIMD_SSE2;
        } else if (strcmp(env, "avx2") == 0 && (caps & PERL_SIMD_CAP_AVX2)) {
            level = PERL_SIMD_AVX2;
        } else if (strcmp(env, "avx512") == 0 &&
                   (caps & PERL_SIMD_CAP_AVX512F) &&
                   (caps & PERL_SIMD_CAP_AVX512BW) &&
                   (caps & PERL_SIMD_CAP_AVX512VL)) {
            level = PERL_SIMD_AVX512;
        } else if (strcmp(env, "vsx") == 0 && (caps & PERL_SIMD_CAP_VSX)) {
            level = PERL_SIMD_VSX;
        } else if (strcmp(env, "vsx2") == 0 && (caps & PERL_SIMD_CAP_VSX2)) {
            level = PERL_SIMD_VSX2;
        } else if (strcmp(env, "neon") == 0 && (caps & PERL_SIMD_CAP_NEON)) {
            level = PERL_SIMD_NEON;
        } else if (strcmp(env, "s390x_vx") == 0 && (caps & PERL_SIMD_CAP_S390X_VX)) {
            level = PERL_SIMD_S390X_VX;
        }
    }

    S_simd_ext_level = level;
    S_simd_caps = caps;
    return level;
}

perl_simd_level_t
Perl_simd_extended_level(void)
{
    if (!S_simd_detected) {
        (void)S_simd_detect_internal();
    }
    return S_simd_ext_level;
}

uint64_t
Perl_simd_caps(void)
{
    if (!S_simd_detected) {
        (void)S_simd_detect_internal();
    }
    return S_simd_caps;
}

#ifndef PERL_SIMD_STANDALONE
void
Perl_simd_detect(pTHX)
{
    perl_simd_level_t ext_level;

    PERL_UNUSED_CONTEXT;

    ext_level = S_simd_detect_internal();
    PL_simd_level = (U8)Perl_simd_baseline_level(ext_level);
}
#else
perl_simd_level_t
Perl_simd_detect_standalone(void)
{
    return S_simd_detect_internal();
}

void
Perl_simd_reset_for_test(void)
{
    S_simd_detected = 0;
    S_simd_ext_level = PERL_SIMD_SCALAR;
    S_simd_caps = 0;
}
#endif

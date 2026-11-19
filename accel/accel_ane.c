/*
 * Apple Silicon Neural Engine (ANE) runtime-probed backend for perl5-mt accel.
 *
 * Detects Apple Silicon arm64 hardware and ANE / CoreML availability via
 * sysctl and framework presence checks.  Dispatch hooks gracefully fall back
 * to Accelerate / scalar twin.
 */

#include "accel_internal.h"

#if defined(__APPLE__) && defined(__aarch64__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <unistd.h>

static int S_ane_inited = 0;

static int
S_ane_probe(void)
{
    int arm64 = 0;
    size_t len = sizeof(arm64);

    if (sysctlbyname("hw.optional.arm64", &arm64, &len, NULL, 0) != 0 || arm64 <= 0) {
        return 0;
    }

    /* Check for AppleNeuralEngine private framework or CoreML framework */
    if (access("/System/Library/PrivateFrameworks/AppleNeuralEngine.framework", F_OK) == 0 ||
        access("/System/Library/Frameworks/CoreML.framework", F_OK) == 0) {
        return 1;
    }
    return 0;
}

static int
S_ane_init(void)
{
    if (!S_ane_probe()) {
        return -1;
    }
    S_ane_inited = 1;
    return 0;
}

static void
S_ane_shutdown(void)
{
    S_ane_inited = 0;
}

static int
S_ane_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    (void)a; (void)b; (void)n; (void)out;
    /* Graceful fallback to Accelerate/NEON or scalar until ANE graph is bound */
    return -1;
}

#else /* !(__APPLE__ && __aarch64__) */

static int  S_ane_probe(void)    { return 0; }
static int  S_ane_init(void)     { return -1; }
static void S_ane_shutdown(void) {}
static int  S_ane_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{ (void)a; (void)b; (void)n; (void)out; return -1; }

#endif

const perl_accel_backend_t perl_accel_backend_ane = {
    .id           = PERL_ACCEL_ANE,
    .name         = "ane",
    .description  = "Apple Silicon Neural Engine (ANE) runtime-probed backend",
    .device_class = PERL_ACCEL_DEV_NPU,
    .priority     = 50,
    .caps         = PERL_ACCEL_CAP_DOT_I8_I32,
    .min_len_f64  = 0,
    .min_len_i8   = 65536u,
    .min_len_nx   = 0,
    .probe        = S_ane_probe,
    .init         = S_ane_init,
    .shutdown     = S_ane_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = NULL,
    .vec_mul_f64  = NULL,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = S_ane_dot_i8_i32,
    .compress_nx  = NULL
};

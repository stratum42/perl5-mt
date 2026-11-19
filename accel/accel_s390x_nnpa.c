/*
 * IBM z16 / Telum II Neural Network Processing Assist (NNPA) backend for
 * perl5-mt accel.
 *
 * Probes s390x facility bit 165 (NNPA) via STFLE on Linux s390x.
 * Gracefully falls back to CPU on non-s390x or pre-z16 systems.
 */

#include "accel_internal.h"

#if defined(__s390x__) && defined(__linux__) && (defined(__GNUC__) || defined(__clang__))
static int
S_s390x_nnpa_probe(void)
{
    uint64_t fac[4] = { 0, 0, 0, 0 };
    register unsigned long r0 __asm__("0") = 3;

    __asm__ __volatile__(
        ".insn s,0xb2b00000,%0\n"
        : "+Q"(fac), "+d"(r0)
        :
        : "cc", "memory"
    );

    /* Facility bit 165 = NNPA: word 2, bit (63 - (165 % 64)) = bit 26 */
    return (fac[165 / 64] & (UINT64_C(1) << (63 - (165 % 64)))) != 0 ? 1 : 0;
}
#else
static int S_s390x_nnpa_probe(void) { return 0; }
#endif

static int
S_s390x_nnpa_init(void)
{
    return S_s390x_nnpa_probe() ? 0 : -1;
}

static void
S_s390x_nnpa_shutdown(void)
{
}

static int
S_s390x_nnpa_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    (void)a; (void)b; (void)n; (void)out;
    return -1; /* graceful fallback until NNPA tensor descriptor path is bound */
}

const perl_accel_backend_t perl_accel_backend_s390x_nnpa = {
    .id           = PERL_ACCEL_S390X_NNPA,
    .name         = "s390x_nnpa",
    .description  = "IBM z16 / Telum II Neural Network Processing Assist (NNPA)",
    .device_class = PERL_ACCEL_DEV_NPU,
    .priority     = 65,
    .caps         = PERL_ACCEL_CAP_DOT_I8_I32,
    .min_len_f64  = 0,
    .min_len_i8   = 4096u,
    .min_len_nx   = 0,
    .probe        = S_s390x_nnpa_probe,
    .init         = S_s390x_nnpa_init,
    .shutdown     = S_s390x_nnpa_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = NULL,
    .vec_mul_f64  = NULL,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = S_s390x_nnpa_dot_i8_i32,
    .compress_nx  = NULL
};

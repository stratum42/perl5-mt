/*
 * IBM POWER Nest Accelerator (NX) coprocessor backend for perl5-mt accel.
 *
 * Probes for /dev/crypto/nx-gzip on Linux ppc64le or POWER9+ NX capability
 * on AIX.  Exposes the compress_nx coprocessor hook with graceful fallback
 * when NX hardware is unavailable.
 */

#include "accel_internal.h"

#if defined(__powerpc64__) || defined(__powerpc__)
#  if defined(__linux__)
#    include <unistd.h>
static int
S_power_nx_probe(void)
{
    return (access("/dev/crypto/nx-gzip", R_OK | W_OK) == 0) ? 1 : 0;
}
#  elif defined(_AIX)
#    include <sys/systemcfg.h>
static int
S_power_nx_probe(void)
{
    return __power_9_andup() ? 1 : 0;
}
#  else
static int S_power_nx_probe(void) { return 0; }
#  endif
#else
static int S_power_nx_probe(void) { return 0; }
#endif

static int
S_power_nx_init(void)
{
    return S_power_nx_probe() ? 0 : -1;
}

static void
S_power_nx_shutdown(void)
{
}

static int
S_power_nx_compress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t *dst_len)
{
    (void)src; (void)src_len; (void)dst; (void)dst_len;
    /* Graceful fallback until libnxz / AIX NX VAS context is bound */
    return -1;
}

const perl_accel_backend_t perl_accel_backend_power_nx = {
    .id           = PERL_ACCEL_POWER_NX,
    .name         = "power_nx",
    .description  = "IBM POWER NX gzip/deflate coprocessor (AIX & Linux ppc64le)",
    .device_class = PERL_ACCEL_DEV_COPROC,
    .priority     = 45,
    .caps         = PERL_ACCEL_CAP_COMPRESS_NX,
    .min_len_f64  = 0,
    .min_len_i8   = 0,
    .min_len_nx   = PERL_ACCEL_DEFAULT_THRESHOLD_NX,
    .probe        = S_power_nx_probe,
    .init         = S_power_nx_init,
    .shutdown     = S_power_nx_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = NULL,
    .vec_mul_f64  = NULL,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = NULL,
    .compress_nx  = S_power_nx_compress
};

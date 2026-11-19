/*
 * AMD XDNA / XRT NPU runtime-probed backend for perl5-mt accel.
 *
 * Probes for AMD XDNA accel device node (/sys/class/accel/accel* vendor
 * 0x1022) and XRT runtime (libxrt_coreutil.so) on Linux x86-64.
 * Gracefully falls back to CPU on hosts without an AMD XDNA NPU.
 */

#include "accel_internal.h"

#if defined(__linux__) && defined(__x86_64__)
#  include <dlfcn.h>
#  include <stdio.h>
#  include <string.h>

static void *S_xrt_dl = NULL;

static int
S_has_amd_xdna_node(void)
{
    int idx;
    for (idx = 0; idx < 4; idx++) {
        char path[128];
        char buf[32] = { 0 };
        FILE *fp;
        snprintf(path, sizeof(path), "/sys/class/accel/accel%d/device/vendor", idx);
        fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        if (fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            if (strstr(buf, "0x1022") != NULL) {
                return 1;
            }
        } else {
            fclose(fp);
        }
    }
    return 0;
}

static int
S_amd_npu_probe(void)
{
    void *h;
    if (!S_has_amd_xdna_node()) {
        return 0;
    }
    h = dlopen("libxrt_coreutil.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        h = dlopen("libxrt_coreutil.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (!h) {
        return 0;
    }
    dlclose(h);
    return 1;
}

static int
S_amd_npu_init(void)
{
    if (!S_has_amd_xdna_node()) {
        return -1;
    }
    S_xrt_dl = dlopen("libxrt_coreutil.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!S_xrt_dl) {
        S_xrt_dl = dlopen("libxrt_coreutil.so", RTLD_LAZY | RTLD_LOCAL);
    }
    return S_xrt_dl ? 0 : -1;
}

static void
S_amd_npu_shutdown(void)
{
    if (S_xrt_dl) {
        dlclose(S_xrt_dl);
        S_xrt_dl = NULL;
    }
}

#else

static int  S_amd_npu_probe(void)    { return 0; }
static int  S_amd_npu_init(void)     { return -1; }
static void S_amd_npu_shutdown(void) {}

#endif

static int
S_amd_npu_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    (void)a; (void)b; (void)n; (void)out;
    return -1; /* graceful fallback to CPU VNNI / scalar */
}

const perl_accel_backend_t perl_accel_backend_amd_npu = {
    .id           = PERL_ACCEL_AMD_NPU,
    .name         = "amd_npu",
    .description  = "AMD XDNA / XRT NPU runtime-probed backend",
    .device_class = PERL_ACCEL_DEV_NPU,
    .priority     = 55,
    .caps         = PERL_ACCEL_CAP_DOT_I8_I32,
    .min_len_f64  = 0,
    .min_len_i8   = 65536u,
    .min_len_nx   = 0,
    .probe        = S_amd_npu_probe,
    .init         = S_amd_npu_init,
    .shutdown     = S_amd_npu_shutdown,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = NULL,
    .vec_mul_f64  = NULL,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = S_amd_npu_dot_i8_i32,
    .compress_nx  = NULL
};

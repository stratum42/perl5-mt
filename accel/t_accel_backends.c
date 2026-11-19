/*
 * Unit tests verifying every runtime-probed backend module (metal, ane,
 * mesa_opencl, intel_npu, amd_npu, power_nx, s390x_nnpa) probes cleanly,
 * handles init/shutdown safely, and falls back to scalar twin without error.
 */

#include "accel_test.h"
#include "perl_accel.h"

#include <errno.h>

int
main(void)
{
    static const char * const names[] = {
        "scalar",
        "accelerate",
        "metal",
        "ane",
        "mesa_opencl",
        "intel_npu",
        "amd_npu",
        "power_nx",
        "s390x_nnpa"
    };
    double a[4] = { 1.5, 2.5, 3.5, 4.5 };
    double b[4] = { 2.0, 2.0, 2.0, 2.0 };
    double out[4];
    int8_t ia[4] = { 10, -20, 30, -40 };
    int8_t ib[4] = { 3, 4, -5, 6 };
    uint8_t src[64] = { 0 };
    uint8_t dst[64] = { 0 };
    size_t dst_len = sizeof(dst);
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        double s;
        int32_t di;

        perl_accel_reset_for_test(names[i], 0);
        ok(perl_accel_active() != NULL,
           "selecting backend '%s' leaves valid active backend (%s)",
           names[i], perl_accel_active()->name);

        s = perl_accel_sum_f64_cr(a, 4);
        is_f64_bits(s, 12.0, "backend '%s' sum_f64_cr returns exact 12.0 (direct or fallback)",
                    names[i]);

        perl_accel_vec_add_f64(a, b, out, 4);
        is_f64_bits(out[0], 3.5, "backend '%s' vec_add_f64[0] == 3.5", names[i]);

        di = perl_accel_dot_i8_i32(ia, ib, 4);
        is_int(di, 10 * 3 + (-20) * 4 + 30 * (-5) + (-40) * 6,
               "backend '%s' dot_i8_i32 exact", names[i]);
    }

    /* compress_nx gracefully returns -ENOTSUP on non-POWER hosts */
    perl_accel_reset_for_test("auto", 0);
    is_int(perl_accel_compress_nx(src, sizeof(src), dst, &dst_len), -ENOTSUP,
           "compress_nx returns -ENOTSUP cleanly when NX coprocessor unavailable");

#if defined(__APPLE__)
    /* On macOS arm64, Accelerate must be available; Metal/ANE probes must not crash */
    is_int(perl_accel_backend_available("accelerate"), 1, "accelerate available on macOS");
    is_int(perl_accel_backend_available("mesa_opencl"), 0, "mesa_opencl unavailable on macOS");
    is_int(perl_accel_backend_available("intel_npu"), 0, "intel_npu unavailable on macOS");
    is_int(perl_accel_backend_available("amd_npu"), 0, "amd_npu unavailable on macOS");
    is_int(perl_accel_backend_available("power_nx"), 0, "power_nx unavailable on macOS");
    is_int(perl_accel_backend_available("s390x_nnpa"), 0, "s390x_nnpa unavailable on macOS");
#endif

    perl_accel_shutdown();
    done_testing();
}

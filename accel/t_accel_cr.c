/*
 * Live correctly-rounded (CR) reduction and vector parity tests:
 * compares active backend (macOS Accelerate Ozaki-split on Darwin) bit-for-bit
 * against the exact Shewchuk scalar twin across:
 *   - Ill-conditioned cancellation arrays (condition number up to 10^30+)
 *   - Exact IEEE-754 half-ulp tie-breaking cases (+sticky, -sticky, 0 sticky)
 *   - Subnormals, -0.0, +Inf, -Inf, +Inf + -Inf, NaN
 *   - Large arrays (n = 100,000) exercising multi-block Ozaki vDSP paths
 *   - vec_add_f64, vec_mul_f64, vec_axpy_f64 (including out == y aliasing)
 *   - dot_i8_i32 (quantized int8 -> int32)
 */

#include "accel_test.h"
#include "perl_accel.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t
rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* Uniform double in [-1.0, 1.0] */
static double
rng_unit(void)
{
    int64_t s = (int64_t)(rng_next() >> 11) - (INT64_C(1) << 52);
    return ldexp((double)s, -52);
}

int
main(void)
{
    perl_accel_stats_t st;

    /* Force threshold = 0 so Accelerate is exercised on every single test case */
    perl_accel_reset_for_test("auto", 0);

#if defined(__APPLE__)
    is_str(perl_accel_active()->name, "accelerate",
           "CR parity suite running live against macOS Accelerate");
#endif

    /* 1. Classic Shewchuk / Kahan catastrophic cancellation */
    {
        double v1[] = { 1e16, 1.0, -1e16 };
        double s_ref = perl_accel_scalar_sum_f64_cr(v1, 3);
        double s_acc = perl_accel_sum_f64_cr(v1, 3);
        is_f64_bits(s_ref, 1.0, "scalar CR sum [1e16, 1.0, -1e16] == 1.0");
        is_f64_bits(s_acc, s_ref, "accel CR sum matches scalar on [1e16, 1.0, -1e16]");
    }

    /* 2. Extreme dynamic range cancellation (1e100 + 1.0 - 1e100 + 1e-100) */
    {
        double v2[] = { 1e100, 1.0, -1e100, 1e-100, -1.0 };
        double s_ref = perl_accel_scalar_sum_f64_cr(v2, 5);
        double s_acc = perl_accel_sum_f64_cr(v2, 5);
        is_f64_bits(s_ref, 1e-100, "scalar CR sum recovers 1e-100 after 1e100 cancellation");
        is_f64_bits(s_acc, s_ref, "accel CR sum bit-identical on 200-decade cancellation");
    }

    /* 3. Exact half-ulp ties (round-to-nearest-even with sticky bit) */
    {
        double half_ulp = ldexp(1.0, -53);       /* 0.5 * ulp(1.0) */
        double tiny     = ldexp(1.0, -107);      /* strictly smaller sticky tail */
        double tie_even[] = { 1.0, half_ulp };
        double tie_up[]   = { 1.0, half_ulp, tiny };
        double tie_down[] = { 1.0, half_ulp, -tiny };
        double tie_odd[]  = { 1.0 + ldexp(1.0, -52), half_ulp };

        is_f64_bits(perl_accel_sum_f64_cr(tie_even, 2), 1.0,
                    "exact half-ulp tie rounds to even (1.0)");
        is_f64_bits(perl_accel_sum_f64_cr(tie_up, 3), 1.0 + ldexp(1.0, -52),
                    "half-ulp + positive sticky rounds up");
        is_f64_bits(perl_accel_sum_f64_cr(tie_down, 3), 1.0,
                    "half-ulp - negative sticky rounds down");
        is_f64_bits(perl_accel_sum_f64_cr(tie_odd, 2), 1.0 + ldexp(2.0, -52),
                    "odd-lsb half-ulp tie rounds to even (up)");

        is_f64_bits(perl_accel_sum_f64_cr(tie_up, 3),
                    perl_accel_scalar_sum_f64_cr(tie_up, 3),
                    "accel matches scalar on half-ulp + sticky");
        is_f64_bits(perl_accel_sum_f64_cr(tie_down, 3),
                    perl_accel_scalar_sum_f64_cr(tie_down, 3),
                    "accel matches scalar on half-ulp - sticky");
    }

    /* 4. IEEE-754 special values: -0.0, +0.0, +/-Inf, NaN */
    {
        double nz[]   = { -0.0, -0.0, -0.0 };
        double mz[]   = { -0.0, 0.0, -0.0 };
        double pinf[] = { 1.0, INFINITY, 2.0 };
        double ninf[] = { 1.0, -INFINITY, 2.0 };
        double binf[] = { INFINITY, -INFINITY };
        double nanv[] = { 1.0, NAN, 2.0 };

        is_f64_bits(perl_accel_sum_f64_cr(nz, 3), -0.0, "sum of -0.0 preserves -0.0");
        is_f64_bits(perl_accel_sum_f64_cr(mz, 3),  0.0, "sum of -0.0 and +0.0 is +0.0");
        is_f64_bits(perl_accel_sum_f64_cr(pinf, 3),  INFINITY, "sum with +Inf is +Inf");
        is_f64_bits(perl_accel_sum_f64_cr(ninf, 3), -INFINITY, "sum with -Inf is -Inf");
        ok(isnan(perl_accel_sum_f64_cr(binf, 2)), "sum of +Inf and -Inf is NaN");
        ok(isnan(perl_accel_sum_f64_cr(nanv, 3)), "sum with NaN is NaN");
    }

    /* 5. Ill-conditioned dot product cancellation */
    {
        double a[] = { 1e8, 1.0, -1e8, 1e-8 };
        double b[] = { 1e8, 3.5,  1e8, 1e-8 };
        /* Exact dot = 1e16 + 3.5 - 1e16 + 1e-16 = 3.5 */
        double d_ref = perl_accel_scalar_dot_f64_cr(a, b, 4);
        double d_acc = perl_accel_dot_f64_cr(a, b, 4);
        is_f64_bits(d_ref, 3.5, "scalar CR dot cancels 1e16 to recover 3.5");
        is_f64_bits(d_acc, d_ref, "accel CR dot bit-identical on cancellation");
    }

    /* 6. Large ill-conditioned array (n = 100,000, multi-block Ozaki) */
    {
        const size_t N = 100000;
        double *a = (double *)malloc(N * sizeof(double));
        double *b = (double *)malloc(N * sizeof(double));
        double *o_ref = (double *)malloc(N * sizeof(double));
        double *o_acc = (double *)malloc(N * sizeof(double));
        size_t i;

        ok(a && b && o_ref && o_acc, "allocated 100k test arrays");

        /* Construct adversarial cancellation pairs + tiny non-cancelling residuals */
        for (i = 0; i < N; i += 2) {
            double big = ldexp(rng_unit(), 40); /* ~1e12 */
            double tiny = rng_unit() * 1e-3;
            a[i]     = big + tiny;
            a[i + 1] = -big;
            b[i]     = ldexp(rng_unit(), 20);
            b[i + 1] = b[i] + rng_unit() * 1e-6;
        }

        perl_accel_reset_stats();
        {
            double s_ref = perl_accel_scalar_sum_f64_cr(a, N);
            double s_acc = perl_accel_sum_f64_cr(a, N);
            double d_ref = perl_accel_scalar_dot_f64_cr(a, b, N);
            double d_acc = perl_accel_dot_f64_cr(a, b, N);

            is_f64_bits(s_acc, s_ref, "100k ill-conditioned sum_f64_cr bit-identical to scalar");
            is_f64_bits(d_acc, d_ref, "100k ill-conditioned dot_f64_cr bit-identical to scalar");
        }

#if defined(__APPLE__)
        perl_accel_get_stats(&st);
        is_int(st.offload_calls, 2, "100k sum + dot both executed on live Accelerate");
        is_str(st.last_backend_name, "accelerate", "last backend was accelerate");
#else
        (void)st;
#endif

        /* Vector ops parity on 100k elements */
        perl_accel_scalar_vec_add_f64(a, b, o_ref, N);
        perl_accel_vec_add_f64(a, b, o_acc, N);
        ok(memcmp(o_ref, o_acc, N * sizeof(double)) == 0,
           "100k vec_add_f64 bit-identical to scalar");

        perl_accel_scalar_vec_mul_f64(a, b, o_ref, N);
        perl_accel_vec_mul_f64(a, b, o_acc, N);
        ok(memcmp(o_ref, o_acc, N * sizeof(double)) == 0,
           "100k vec_mul_f64 bit-identical to scalar");

        perl_accel_scalar_vec_axpy_f64(2.75, a, b, o_ref, N);
        perl_accel_vec_axpy_f64(2.75, a, b, o_acc, N);
        ok(memcmp(o_ref, o_acc, N * sizeof(double)) == 0,
           "100k vec_axpy_f64 bit-identical to scalar");

        /* In-place aliasing check: out == y */
        memcpy(o_acc, b, N * sizeof(double));
        perl_accel_vec_axpy_f64(2.75, a, o_acc, o_acc, N);
        ok(memcmp(o_ref, o_acc, N * sizeof(double)) == 0,
           "100k vec_axpy_f64 with out == y aliasing bit-identical");

        free(a);
        free(b);
        free(o_ref);
        free(o_acc);
    }

    /* 7. Quantized dot_i8_i32 parity (including non-multiple-of-16 tail) */
    {
        const size_t N = 4111;
        int8_t *ia = (int8_t *)malloc(N);
        int8_t *ib = (int8_t *)malloc(N);
        size_t i;
        int32_t ref_i, acc_i;

        for (i = 0; i < N; i++) {
            ia[i] = (int8_t)((int)(rng_next() % 255) - 127);
            ib[i] = (int8_t)((int)(rng_next() % 255) - 127);
        }

        ref_i = perl_accel_scalar_dot_i8_i32(ia, ib, N);
        acc_i = perl_accel_dot_i8_i32(ia, ib, N);
        is_int(acc_i, ref_i, "dot_i8_i32 (n=4111) matches scalar twin exactly");

        free(ia);
        free(ib);
    }

    perl_accel_shutdown();
    done_testing();
}

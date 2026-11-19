/*
 * macOS Accelerate (vDSP / BLAS) backend for perl5-mt accel.
 *
 * Implements:
 *   - sum_f64_cr: Ozaki-style exponent-slice splitting over vDSP_sveD with
 *     Shewchuk expansion accumulation + exact IEEE-754 RNE rounding.
 *   - dot_f64_cr: Ozaki-style operand splitting over vDSP_dotprD so every
 *     partial product and slice dot product has zero rounding error
 *     regardless of BLAS/vDSP summation order.
 *   - vec_add_f64, vec_mul_f64, vec_axpy_f64: vDSP vectorized double ops.
 *   - dot_i8_i32: ARM64 NEON vectorized int8->int32 dot product.
 *
 * Pure C11 + <Accelerate/Accelerate.h> (no Objective-C).
 * On non-Darwin hosts, compiles as an unavailable stub.
 */

#pragma STDC FP_CONTRACT OFF

#include "accel_internal.h"

#if defined(__APPLE__)

#include <Accelerate/Accelerate.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#  include <arm_neon.h>
#endif

/* Block size B = 2^14 = 16384 (128 KiB per double buffer, L2-resident) */
#define OZAKI_BLOCK_LEN    16384u
#define OZAKI_SUM_MAX_SLICES 6u
#define OZAKI_DOT_MAX_SLICES 4u

static int  S_accel_probe(void)    { return 1; }
static int  S_accel_init(void)     { return 0; }
static void S_accel_shutdown(void) {}

static int
S_accel_sum_f64_cr(const double *a, size_t n, double *out)
{
    double max_all = 0.0;
    double *r_buf = NULL;
    double *q_buf = NULL;
    size_t offset;
    perl_accel_exp_t acc;
    double res;

    if (!a || n == 0) {
        *out = 0.0;
        return 0;
    }

    /* Fast vDSP scan for global max magnitude; delegate special/extreme cases */
    vDSP_maxmgvD(a, 1, &max_all, (vDSP_Length)n);
    if (!isfinite(max_all) || max_all == 0.0 || max_all > 1e280 || max_all < 1e-280) {
        *out = perl_accel_scalar_sum_f64_cr(a, n);
        return 0;
    }

    /* Verify no NaN slipped past maxmgvD */
    {
        double min_v = 0.0, max_v = 0.0;
        vDSP_minvD(a, 1, &min_v, (vDSP_Length)n);
        vDSP_maxvD(a, 1, &max_v, (vDSP_Length)n);
        if (!isfinite(min_v) || !isfinite(max_v)) {
            *out = perl_accel_scalar_sum_f64_cr(a, n);
            return 0;
        }
    }

    r_buf = (double *)malloc(OZAKI_BLOCK_LEN * sizeof(double));
    q_buf = (double *)malloc(OZAKI_BLOCK_LEN * sizeof(double));
    if (!r_buf || !q_buf) {
        free(r_buf);
        free(q_buf);
        return -1; /* fall back to scalar */
    }

    perl_accel_exp_init(&acc);

    for (offset = 0; offset < n; offset += OZAKI_BLOCK_LEN) {
        size_t len = n - offset;
        if (len > OZAKI_BLOCK_LEN) {
            len = OZAKI_BLOCK_LEN;
        }
        memcpy(r_buf, a + offset, len * sizeof(double));

        size_t s;
        double blk_max = 0.0;
        for (s = 0; s < OZAKI_SUM_MAX_SLICES; s++) {
            int e;
            double sigma, neg_sigma, slice_sum = 0.0;

            vDSP_maxmgvD(r_buf, 1, &blk_max, (vDSP_Length)len);
            if (blk_max == 0.0) {
                break;
            }
            if (!isfinite(blk_max) || blk_max > 1e280 || blk_max < 1e-280) {
                /* Extreme residual: accumulate directly */
                break;
            }

            /*
             * Extract p = 36 bits per slice:
             * sigma = 1.5 * 2^(e + 52 - 36) = 1.5 * 2^(e + 16).
             * Every q[i] is an exact multiple of 2^(e - 36) with |q[i]| <= 2^(e+1).
             * Summing <= 2^14 terms spans <= 37 + 14 = 51 <= 53 bits: EXACT.
             */
            e = ilogb(blk_max);
            sigma = ldexp(1.5, e + 16);
            neg_sigma = -sigma;

            vDSP_vsaddD(r_buf, 1, &sigma, q_buf, 1, (vDSP_Length)len);
            vDSP_vsaddD(q_buf, 1, &neg_sigma, q_buf, 1, (vDSP_Length)len);
            /* vDSP_vsubD(B, 1, A, 1, C, 1, N) computes C = A - B */
            vDSP_vsubD(q_buf, 1, r_buf, 1, r_buf, 1, (vDSP_Length)len);

            vDSP_sveD(q_buf, 1, &slice_sum, (vDSP_Length)len);
            perl_accel_exp_add(&acc, slice_sum);
        }

        /* Fold any remaining ultra-wide dynamic range residual exactly */
        vDSP_maxmgvD(r_buf, 1, &blk_max, (vDSP_Length)len);
        if (blk_max != 0.0) {
            size_t i;
            for (i = 0; i < len; i++) {
                if (r_buf[i] != 0.0) {
                    perl_accel_exp_add(&acc, r_buf[i]);
                }
            }
        }
    }

    res = perl_accel_exp_round_rne(&acc);
    perl_accel_exp_free(&acc);
    free(r_buf);
    free(q_buf);

    if (res == 0.0) {
        res = 0.0;
    }
    *out = res;
    return 0;
}

/* Split a block into up to OZAKI_DOT_MAX_SLICES 18-bit slices + exact remainder */
static size_t
S_ozaki_split_18(const double *src, size_t len,
                 double *slices[OZAKI_DOT_MAX_SLICES],
                 double *rem, int *rem_nonzero_out)
{
    size_t k = 0;
    double blk_max = 0.0;

    memcpy(rem, src, len * sizeof(double));
    *rem_nonzero_out = 0;

    while (k < OZAKI_DOT_MAX_SLICES) {
        int e;
        double sigma, neg_sigma;

        vDSP_maxmgvD(rem, 1, &blk_max, (vDSP_Length)len);
        if (blk_max == 0.0) {
            return k;
        }
        if (!isfinite(blk_max) || blk_max > 1e130 || blk_max < 1e-130) {
            *rem_nonzero_out = 1;
            return k;
        }

        /*
         * Extract p_d = 18 bits per slice:
         * sigma = 1.5 * 2^(e + 52 - 18) = 1.5 * 2^(e + 34).
         * Every product a_r[i]*b_s[i] is exact (36 bits) and summing <= 2^14
         * products spans <= 38 + 14 = 52 <= 53 bits: ZERO rounding error.
         */
        e = ilogb(blk_max);
        sigma = ldexp(1.5, e + 34);
        neg_sigma = -sigma;

        vDSP_vsaddD(rem, 1, &sigma, slices[k], 1, (vDSP_Length)len);
        vDSP_vsaddD(slices[k], 1, &neg_sigma, slices[k], 1, (vDSP_Length)len);
        vDSP_vsubD(slices[k], 1, rem, 1, rem, 1, (vDSP_Length)len);
        k++;
    }

    vDSP_maxmgvD(rem, 1, &blk_max, (vDSP_Length)len);
    if (blk_max != 0.0) {
        *rem_nonzero_out = 1;
    }
    return k;
}

static int
S_accel_dot_f64_cr(const double *a, const double *b, size_t n, double *out)
{
    double max_a = 0.0, max_b = 0.0;
    double *pool = NULL;
    double *a_slices[OZAKI_DOT_MAX_SLICES];
    double *b_slices[OZAKI_DOT_MAX_SLICES];
    double *a_rem, *b_rem, *tmp;
    size_t offset, i;
    perl_accel_exp_t acc;
    double res;

    if (!a || !b || n == 0) {
        *out = 0.0;
        return 0;
    }

    vDSP_maxmgvD(a, 1, &max_a, (vDSP_Length)n);
    vDSP_maxmgvD(b, 1, &max_b, (vDSP_Length)n);
    if (!isfinite(max_a) || !isfinite(max_b) ||
        max_a == 0.0 || max_b == 0.0 ||
        max_a > 1e130 || max_b > 1e130 ||
        max_a < 1e-130 || max_b < 1e-130) {
        *out = perl_accel_scalar_dot_f64_cr(a, b, n);
        return 0;
    }

    /* Check for NaNs */
    {
        double min_a = 0.0, max_va = 0.0, min_b = 0.0, max_vb = 0.0;
        vDSP_minvD(a, 1, &min_a, (vDSP_Length)n);
        vDSP_maxvD(a, 1, &max_va, (vDSP_Length)n);
        vDSP_minvD(b, 1, &min_b, (vDSP_Length)n);
        vDSP_maxvD(b, 1, &max_vb, (vDSP_Length)n);
        if (!isfinite(min_a) || !isfinite(max_va) ||
            !isfinite(min_b) || !isfinite(max_vb)) {
            *out = perl_accel_scalar_dot_f64_cr(a, b, n);
            return 0;
        }
    }

    /* Single contiguous scratch allocation: 2*4 slices + a_rem + b_rem + tmp = 11 blocks */
    pool = (double *)malloc((2 * OZAKI_DOT_MAX_SLICES + 3) * OZAKI_BLOCK_LEN * sizeof(double));
    if (!pool) {
        return -1;
    }
    for (i = 0; i < OZAKI_DOT_MAX_SLICES; i++) {
        a_slices[i] = pool + i * OZAKI_BLOCK_LEN;
        b_slices[i] = pool + (OZAKI_DOT_MAX_SLICES + i) * OZAKI_BLOCK_LEN;
    }
    a_rem = pool + (2 * OZAKI_DOT_MAX_SLICES) * OZAKI_BLOCK_LEN;
    b_rem = pool + (2 * OZAKI_DOT_MAX_SLICES + 1) * OZAKI_BLOCK_LEN;
    tmp   = pool + (2 * OZAKI_DOT_MAX_SLICES + 2) * OZAKI_BLOCK_LEN;

    perl_accel_exp_init(&acc);

    for (offset = 0; offset < n; offset += OZAKI_BLOCK_LEN) {
        size_t len = n - offset;
        const double *a_blk = a + offset;
        const double *b_blk = b + offset;
        int a_rem_nz = 0, b_rem_nz = 0;
        size_t ka, kb, r, s;

        if (len > OZAKI_BLOCK_LEN) {
            len = OZAKI_BLOCK_LEN;
        }

        ka = S_ozaki_split_18(a_blk, len, a_slices, a_rem, &a_rem_nz);
        kb = S_ozaki_split_18(b_blk, len, b_slices, b_rem, &b_rem_nz);

        /* Exact slice-pair dot products via vDSP_dotprD */
        for (r = 0; r < ka; r++) {
            for (s = 0; s < kb; s++) {
                double part = 0.0;
                vDSP_dotprD(a_slices[r], 1, b_slices[s], 1, &part, (vDSP_Length)len);
                perl_accel_exp_add(&acc, part);
            }
        }

        /* Exact residual cross-terms for extreme dynamic range */
        if (a_rem_nz) {
            for (i = 0; i < len; i++) {
                if (a_rem[i] != 0.0 && b_blk[i] != 0.0) {
                    perl_accel_exp_add_prod(&acc, a_rem[i], b_blk[i]);
                }
            }
        }
        if (b_rem_nz) {
            /* tmp = a_blk - a_rem so a_rem * b_rem is not double-counted */
            if (a_rem_nz) {
                vDSP_vsubD(a_rem, 1, a_blk, 1, tmp, 1, (vDSP_Length)len);
            } else {
                memcpy(tmp, a_blk, len * sizeof(double));
            }
            for (i = 0; i < len; i++) {
                if (tmp[i] != 0.0 && b_rem[i] != 0.0) {
                    perl_accel_exp_add_prod(&acc, tmp[i], b_rem[i]);
                }
            }
        }
    }

    res = perl_accel_exp_round_rne(&acc);
    perl_accel_exp_free(&acc);
    free(pool);

    if (res == 0.0) {
        res = 0.0;
    }
    *out = res;
    return 0;
}

static int
S_accel_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{
    if (n > 0) {
        vDSP_vaddD(a, 1, b, 1, out, 1, (vDSP_Length)n);
    }
    return 0;
}

static int
S_accel_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{
    if (n > 0) {
        vDSP_vmulD(a, 1, b, 1, out, 1, (vDSP_Length)n);
    }
    return 0;
}

static int
S_accel_vec_axpy_f64(double alpha, const double *x, const double *y,
                     double *out, size_t n)
{
    if (n == 0) {
        return 0;
    }
    /*
     * Separate multiply + add preserves bit-identical IEEE-754 semantics
     * with the non-FMA scalar twin.  Handle out == y aliasing safely.
     */
    if (out == y) {
        double buf[1024];
        size_t off = 0;
        while (off < n) {
            size_t chunk = n - off;
            if (chunk > 1024) chunk = 1024;
            vDSP_vsmulD(x + off, 1, &alpha, buf, 1, (vDSP_Length)chunk);
            vDSP_vaddD(buf, 1, y + off, 1, out + off, 1, (vDSP_Length)chunk);
            off += chunk;
        }
    } else {
        vDSP_vsmulD(x, 1, &alpha, out, 1, (vDSP_Length)n);
        vDSP_vaddD(out, 1, y, 1, out, 1, (vDSP_Length)n);
    }
    return 0;
}

static int
S_accel_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    size_t i = 0;
    int32_t acc = 0;

#if defined(__aarch64__)
    int32x4_t vsum0 = vdupq_n_s32(0);
    int32x4_t vsum1 = vdupq_n_s32(0);

    for (; i + 16 <= n; i += 16) {
        int8x16_t va = vld1q_s8(a + i);
        int8x16_t vb = vld1q_s8(b + i);
#  if defined(__ARM_FEATURE_DOTPROD)
        vsum0 = vdotq_s32(vsum0, va, vb);
#  else
        int16x8_t p_lo = vmull_s8(vget_low_s8(va), vget_low_s8(vb));
        int16x8_t p_hi = vmull_s8(vget_high_s8(va), vget_high_s8(vb));
        vsum0 = vpadalq_s16(vsum0, p_lo);
        vsum1 = vpadalq_s16(vsum1, p_hi);
#  endif
    }

    acc = vaddvq_s32(vaddq_s32(vsum0, vsum1));
#endif

    for (; i < n; i++) {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
    *out = acc;
    return 0;
}

const perl_accel_backend_t perl_accel_backend_accelerate = {
    .id           = PERL_ACCEL_ACCELERATE,
    .name         = "accelerate",
    .description  = "macOS Accelerate (vDSP/BLAS) with Ozaki CR reductions",
    .device_class = PERL_ACCEL_DEV_CPU,
    .priority     = 90,
    .caps         = PERL_ACCEL_CAP_SUM_F64_CR |
                    PERL_ACCEL_CAP_DOT_F64_CR |
                    PERL_ACCEL_CAP_VEC_ADD_F64 |
                    PERL_ACCEL_CAP_VEC_MUL_F64 |
                    PERL_ACCEL_CAP_VEC_AXPY_F64 |
                    PERL_ACCEL_CAP_DOT_I8_I32,
    .min_len_f64  = PERL_ACCEL_DEFAULT_THRESHOLD_F64,
    .min_len_i8   = PERL_ACCEL_DEFAULT_THRESHOLD_I8,
    .min_len_nx   = 0,
    .probe        = S_accel_probe,
    .init         = S_accel_init,
    .shutdown     = S_accel_shutdown,
    .sum_f64_cr   = S_accel_sum_f64_cr,
    .dot_f64_cr   = S_accel_dot_f64_cr,
    .vec_add_f64  = S_accel_vec_add_f64,
    .vec_mul_f64  = S_accel_vec_mul_f64,
    .vec_axpy_f64 = S_accel_vec_axpy_f64,
    .dot_i8_i32   = S_accel_dot_i8_i32,
    .compress_nx  = NULL
};

#else /* !__APPLE__ */

static int S_accel_probe_stub(void) { return 0; }

const perl_accel_backend_t perl_accel_backend_accelerate = {
    .id           = PERL_ACCEL_ACCELERATE,
    .name         = "accelerate",
    .description  = "macOS Accelerate (unavailable on this platform)",
    .device_class = PERL_ACCEL_DEV_CPU,
    .priority     = 90,
    .caps         = 0,
    .min_len_f64  = PERL_ACCEL_DEFAULT_THRESHOLD_F64,
    .min_len_i8   = PERL_ACCEL_DEFAULT_THRESHOLD_I8,
    .min_len_nx   = 0,
    .probe        = S_accel_probe_stub,
    .init         = NULL,
    .shutdown     = NULL,
    .sum_f64_cr   = NULL,
    .dot_f64_cr   = NULL,
    .vec_add_f64  = NULL,
    .vec_mul_f64  = NULL,
    .vec_axpy_f64 = NULL,
    .dot_i8_i32   = NULL,
    .compress_nx  = NULL
};

#endif /* __APPLE__ */

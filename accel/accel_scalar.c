/*
 * Reference scalar backend and exact Shewchuk / compensated correctly-rounded
 * arithmetic twin for perl5-mt accel.
 *
 * FP_CONTRACT is explicitly disabled so Knuth TwoSum and Shewchuk expansion
 * operations retain exact IEEE-754 binary64 rounding semantics on all targets.
 */

#pragma STDC FP_CONTRACT OFF

#include "accel_internal.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Error-Free Transformations (Knuth TwoSum, Dekker/FMA TwoProd) -------- */

static inline void
S_two_sum(double a, double b, double *s, double *e)
{
    double x = a + b;
    double bv = x - a;
    double av = x - bv;
    *s = x;
    *e = (a - av) + (b - bv);
}

static inline void
S_fast_two_sum(double a, double b, double *s, double *e)
{
    double x = a + b;
    *s = x;
    *e = b - (x - a);
}

static inline void
S_two_prod(double a, double b, double *p, double *e)
{
    double x = a * b;
    *p = x;
    *e = fma(a, b, -x);
}

/* ---- Exact Shewchuk Adaptive Floating-Point Expansion --------------------- */

void
perl_accel_exp_init(perl_accel_exp_t *acc)
{
    acc->e = acc->inline_buf;
    acc->len = 0;
    acc->cap = PERL_ACCEL_EXP_INLINE_CAP;
    acc->oom = 0;
}

void
perl_accel_exp_free(perl_accel_exp_t *acc)
{
    if (acc->e && acc->e != acc->inline_buf) {
        free(acc->e);
    }
    acc->e = NULL;
    acc->len = 0;
    acc->cap = 0;
}

static int
S_exp_ensure(perl_accel_exp_t *acc, size_t need)
{
    size_t new_cap;
    double *p;

    if (need <= acc->cap) {
        return 0;
    }
    new_cap = acc->cap * 2;
    while (new_cap < need) {
        new_cap *= 2;
    }
    if (acc->e == acc->inline_buf) {
        p = (double *)malloc(new_cap * sizeof(double));
        if (!p) {
            acc->oom = 1;
            return -1;
        }
        memcpy(p, acc->inline_buf, acc->len * sizeof(double));
    } else {
        p = (double *)realloc(acc->e, new_cap * sizeof(double));
        if (!p) {
            acc->oom = 1;
            return -1;
        }
    }
    acc->e = p;
    acc->cap = new_cap;
    return 0;
}

/* Shewchuk Theorem 23: Compress a non-overlapping expansion in-place */
static void
S_exp_compress(perl_accel_exp_t *acc)
{
    double *e = acc->e;
    size_t n = acc->len;
    double q, h, Q;
    size_t bottom, top, i;

    if (n <= 1) {
        return;
    }

    /* Top-down pass */
    bottom = n - 1;
    q = e[bottom];
    for (i = n - 1; i > 0; i--) {
        S_two_sum(q, e[i - 1], &Q, &h);
        if (h != 0.0) {
            e[bottom] = Q;
            bottom--;
            q = h;
        } else {
            q = Q;
        }
    }
    e[bottom] = q;

    /* Bottom-up pass */
    top = 0;
    for (i = bottom + 1; i < n; i++) {
        S_two_sum(e[i], q, &Q, &h);
        if (h != 0.0) {
            e[top++] = h;
        }
        q = Q;
    }
    e[top++] = q;
    acc->len = top;
}

/* Grow-Expansion: exact addition of x to expansion acc */
void
perl_accel_exp_add(perl_accel_exp_t *acc, double x)
{
    size_t i, out = 0;
    double q = x;

    if (x == 0.0 || acc->oom) {
        return;
    }

    for (i = 0; i < acc->len; i++) {
        double s, err;
        S_two_sum(q, acc->e[i], &s, &err);
        q = s;
        if (err != 0.0) {
            acc->e[out++] = err;
        }
    }

    if (q != 0.0) {
        if (out >= acc->cap) {
            if (S_exp_ensure(acc, out + 1) != 0) {
                return;
            }
        }
        acc->e[out++] = q;
    }
    acc->len = out;

    /* Periodically compress to keep expansion compact (O(1) typical length) */
    if (acc->len >= 48) {
        S_exp_compress(acc);
    }
}

void
perl_accel_exp_add_prod(perl_accel_exp_t *acc, double a, double b)
{
    double p, err;
    if (a == 0.0 || b == 0.0 || acc->oom) {
        return;
    }
    S_two_prod(a, b, &p, &err);
    perl_accel_exp_add(acc, p);
    if (err != 0.0) {
        perl_accel_exp_add(acc, err);
    }
}

/*
 * Exact IEEE-754 binary64 round-to-nearest-even of a Shewchuk expansion.
 *
 * After two compression passes, e[len-1] = hi, e[len-2] = lo, e[len-3] = s
 * satisfy |lo| <= 0.5*ulp(hi) and |s| <= 0.5*ulp(lo) with hi + lo == hi
 * except at exact half-ulp ties.  An exact half-ulp of any binary64 value is
 * always a power of two (zero mantissa bits).  Only when lo is an exact power
 * of two and the residual tail s is nonzero can s break a tie; nudging lo by
 * one ULP in the direction of s via nextafter() produces the exact RNE sum.
 */
double
perl_accel_exp_round_rne(perl_accel_exp_t *acc)
{
    double hi, lo, s;
    uint64_t lo_bits;

    if (acc->len == 0) {
        return 0.0;
    }

    S_exp_compress(acc);
    S_exp_compress(acc);

    if (acc->len == 0) {
        return 0.0;
    }
    if (acc->len == 1) {
        return acc->e[0];
    }

    hi = acc->e[acc->len - 1];
    lo = acc->e[acc->len - 2];

    if (acc->len >= 3) {
        s = acc->e[acc->len - 3];
        if (s != 0.0 && lo != 0.0) {
            memcpy(&lo_bits, &lo, sizeof(lo_bits));
            /* Check if lo is an exact power of 2 (mantissa == 0, normal) */
            if ((lo_bits & UINT64_C(0x000FFFFFFFFFFFFF)) == 0 &&
                (lo_bits & UINT64_C(0x7FF0000000000000)) != 0) {
                lo = nextafter(lo, copysign(INFINITY, s));
            }
        }
    }

    /*
     * Re-run FastTwoSum on (hi, lo) so if lo is > 0.5*ulp(hi) it rounds
     * cleanly, and if lo == 0.5*ulp(hi) it uses IEEE-754 ties-to-even.
     */
    {
        double res, dummy;
        S_fast_two_sum(hi, lo, &res, &dummy);
        (void)dummy;
        return res;
    }
}

/* ---- Direct Reference Scalar Twin Operations ------------------------------ */

double
perl_accel_scalar_sum_f64_cr(const double *a, size_t n)
{
    size_t i;
    int has_pos_inf = 0, has_neg_inf = 0;
    int all_neg_zero = 1;
    double max_abs = 0.0;
    int scale_exp = 0;
    perl_accel_exp_t acc;
    double res;

    if (n == 0 || !a) {
        return 0.0;
    }

    for (i = 0; i < n; i++) {
        double v = a[i];
        if (isnan(v)) {
            return NAN;
        }
        if (isinf(v)) {
            if (v > 0.0) has_pos_inf = 1;
            else         has_neg_inf = 1;
        } else {
            double av = fabs(v);
            if (av > max_abs) {
                max_abs = av;
            }
            if (v != 0.0 || !signbit(v)) {
                all_neg_zero = 0;
            }
        }
    }

    if (has_pos_inf || has_neg_inf) {
        if (has_pos_inf && has_neg_inf) {
            return NAN;
        }
        return has_pos_inf ? INFINITY : -INFINITY;
    }

    if (max_abs == 0.0) {
        return all_neg_zero ? -0.0 : 0.0;
    }

    /* Prevent intermediate TwoSum overflow near DBL_MAX */
    if (max_abs > 1e290) {
        scale_exp = -64;
    }

    perl_accel_exp_init(&acc);
    if (scale_exp == 0) {
        for (i = 0; i < n; i++) {
            perl_accel_exp_add(&acc, a[i]);
        }
    } else {
        for (i = 0; i < n; i++) {
            perl_accel_exp_add(&acc, ldexp(a[i], scale_exp));
        }
    }

    res = perl_accel_exp_round_rne(&acc);
    perl_accel_exp_free(&acc);

    if (scale_exp != 0) {
        res = ldexp(res, -scale_exp);
    }

    /* Exact cancellation of nonzero finite values produces +0.0 per IEEE-754 */
    if (res == 0.0) {
        return 0.0;
    }
    return res;
}

double
perl_accel_scalar_dot_f64_cr(const double *a, const double *b, size_t n)
{
    size_t i;
    int has_pos_inf = 0, has_neg_inf = 0;
    int all_neg_zero = 1;
    double max_a = 0.0, max_b = 0.0;
    int scale_a = 0, scale_b = 0;
    perl_accel_exp_t acc;
    double res;

    if (n == 0 || !a || !b) {
        return 0.0;
    }

    for (i = 0; i < n; i++) {
        double va = a[i], vb = b[i];
        double p = va * vb;
        if (isnan(va) || isnan(vb) || isnan(p)) {
            return NAN;
        }
        if (isinf(va) || isinf(vb)) {
            if (p > 0.0) has_pos_inf = 1;
            else         has_neg_inf = 1;
        } else {
            double aa = fabs(va), ab = fabs(vb);
            if (aa > max_a) max_a = aa;
            if (ab > max_b) max_b = ab;
            if (p != 0.0 || !signbit(p)) {
                all_neg_zero = 0;
            }
        }
    }

    if (has_pos_inf || has_neg_inf) {
        if (has_pos_inf && has_neg_inf) {
            return NAN;
        }
        return has_pos_inf ? INFINITY : -INFINITY;
    }

    if (max_a == 0.0 || max_b == 0.0) {
        return all_neg_zero ? -0.0 : 0.0;
    }

    /* Prevent intermediate product overflow or subnormal underflow in TwoProd */
    if (max_a > 1e140) scale_a = -512;
    else if (max_a < 1e-140) scale_a = 512;

    if (max_b > 1e140) scale_b = -512;
    else if (max_b < 1e-140) scale_b = 512;

    perl_accel_exp_init(&acc);
    if (scale_a == 0 && scale_b == 0) {
        for (i = 0; i < n; i++) {
            perl_accel_exp_add_prod(&acc, a[i], b[i]);
        }
    } else {
        for (i = 0; i < n; i++) {
            double sa = (scale_a != 0) ? ldexp(a[i], scale_a) : a[i];
            double sb = (scale_b != 0) ? ldexp(b[i], scale_b) : b[i];
            perl_accel_exp_add_prod(&acc, sa, sb);
        }
    }

    res = perl_accel_exp_round_rne(&acc);
    perl_accel_exp_free(&acc);

    if (scale_a + scale_b != 0) {
        res = ldexp(res, -(scale_a + scale_b));
    }

    if (res == 0.0) {
        return 0.0;
    }
    return res;
}

void
perl_accel_scalar_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

void
perl_accel_scalar_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        out[i] = a[i] * b[i];
    }
}

void
perl_accel_scalar_vec_axpy_f64(double alpha, const double *x, const double *y,
                               double *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        double prod = alpha * x[i];
        out[i] = prod + y[i];
    }
}

int32_t
perl_accel_scalar_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n)
{
    size_t i;
    int32_t acc = 0;
    for (i = 0; i < n; i++) {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
    return acc;
}

/* ---- Scalar Backend Vtable Wrappers --------------------------------------- */

static int S_scalar_probe(void) { return 1; }
static int S_scalar_init(void)  { return 0; }
static void S_scalar_shutdown(void) {}

static int
S_scalar_sum_f64_cr(const double *a, size_t n, double *out)
{
    *out = perl_accel_scalar_sum_f64_cr(a, n);
    return 0;
}

static int
S_scalar_dot_f64_cr(const double *a, const double *b, size_t n, double *out)
{
    *out = perl_accel_scalar_dot_f64_cr(a, b, n);
    return 0;
}

static int
S_scalar_vec_add_f64(const double *a, const double *b, double *out, size_t n)
{
    perl_accel_scalar_vec_add_f64(a, b, out, n);
    return 0;
}

static int
S_scalar_vec_mul_f64(const double *a, const double *b, double *out, size_t n)
{
    perl_accel_scalar_vec_mul_f64(a, b, out, n);
    return 0;
}

static int
S_scalar_vec_axpy_f64(double alpha, const double *x, const double *y,
                      double *out, size_t n)
{
    perl_accel_scalar_vec_axpy_f64(alpha, x, y, out, n);
    return 0;
}

static int
S_scalar_dot_i8_i32(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    *out = perl_accel_scalar_dot_i8_i32(a, b, n);
    return 0;
}

const perl_accel_backend_t perl_accel_backend_scalar = {
    .id           = PERL_ACCEL_SCALAR,
    .name         = "scalar",
    .description  = "Exact reference scalar twin (Shewchuk CR reductions)",
    .device_class = PERL_ACCEL_DEV_CPU,
    .priority     = 0,
    .caps         = PERL_ACCEL_CAP_SUM_F64_CR |
                    PERL_ACCEL_CAP_DOT_F64_CR |
                    PERL_ACCEL_CAP_VEC_ADD_F64 |
                    PERL_ACCEL_CAP_VEC_MUL_F64 |
                    PERL_ACCEL_CAP_VEC_AXPY_F64 |
                    PERL_ACCEL_CAP_DOT_I8_I32,
    .min_len_f64  = 0,
    .min_len_i8   = 0,
    .min_len_nx   = 0,
    .probe        = S_scalar_probe,
    .init         = S_scalar_init,
    .shutdown     = S_scalar_shutdown,
    .sum_f64_cr   = S_scalar_sum_f64_cr,
    .dot_f64_cr   = S_scalar_dot_f64_cr,
    .vec_add_f64  = S_scalar_vec_add_f64,
    .vec_mul_f64  = S_scalar_vec_mul_f64,
    .vec_axpy_f64 = S_scalar_vec_axpy_f64,
    .dot_i8_i32   = S_scalar_dot_i8_i32,
    .compress_nx  = NULL
};

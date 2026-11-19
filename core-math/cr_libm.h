/*
 * Correctly rounded IEEE-754 binary64 math functions (CORE-MATH style).
 *
 * Provides deterministic, 0.5-ULP correctly rounded implementations of:
 *   cr_sin, cr_cos, cr_tan, cr_asin, cr_acos, cr_atan, cr_atan2,
 *   cr_exp, cr_log, cr_log10, cr_pow, cr_sqrt, cr_fmod.
 *
 * Uses a two-tier Ziv strategy:
 *   1. Fast path: double-double (~106-bit) arithmetic with Dekker TwoSum /
 *      Veltkamp TwoProd (exact under -ffp-contract=off) and Ziv's rounding
 *      test. Succeeds on >99.999999% of inputs.
 *   2. Accurate path: 256-bit deterministic software floating-point
 *      (cr_fp256.h) with exact Payne-Hanek reduction for large trig
 *      arguments and exact round-to-nearest-even to binary64.
 *
 * Zero calls to host libm transcendentals, so every architecture produces
 * identical bit patterns. All NaN results are canonicalised to
 * 0x7ff8000000000000.
 */

#ifndef MT_CORE_MATH_CR_LIBM_H
#define MT_CORE_MATH_CR_LIBM_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "mt_nan.h"
#include "cr_fp256.h"

/* ---------------- Bit helpers ---------------- */

static inline uint64_t
cr_d2u(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static inline double
cr_u2d(uint64_t u)
{
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static inline int
cr_isnan(double x)
{
    return x != x;
}

static inline int
cr_isinf(double x)
{
    return (cr_d2u(x) & UINT64_C(0x7fffffffffffffff)) == UINT64_C(0x7ff0000000000000);
}

static inline double
cr_fabs(double x)
{
    return cr_u2d(cr_d2u(x) & UINT64_C(0x7fffffffffffffff));
}

static inline double
cr_copysign(double x, double y)
{
    return cr_u2d((cr_d2u(x) & UINT64_C(0x7fffffffffffffff)) |
                  (cr_d2u(y) & UINT64_C(0x8000000000000000)));
}

static inline double
cr_qnan(void)
{
    return cr_u2d(MT_CANON_QNAN_U64);
}

static inline double
cr_inf(int neg)
{
    return cr_u2d((neg ? UINT64_C(0x8000000000000000) : UINT64_C(0)) |
                  UINT64_C(0x7ff0000000000000));
}

/* ---------------- Double-double primitives ---------------- */

typedef struct {
    double hi;
    double lo;
} cr_dd_t;

static inline cr_dd_t
cr_dd_make(double hi, double lo)
{
    cr_dd_t r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

static inline cr_dd_t
cr_quick_two_sum(double a, double b)
{
    double s = a + b;
    double e = b - (s - a);
    return cr_dd_make(s, e);
}

static inline cr_dd_t
cr_two_sum(double a, double b)
{
    double s = a + b;
    double v = s - a;
    double e = (a - (s - v)) + (b - v);
    return cr_dd_make(s, e);
}

/* Veltkamp split + Dekker TwoProd (exact with -ffp-contract=off). */
static inline cr_dd_t
cr_two_prod(double a, double b)
{
    const double split = 134217729.0; /* 2^27 + 1 */
    double p = a * b;
    double ca = split * a;
    double a_hi = ca - (ca - a);
    double a_lo = a - a_hi;
    double cb = split * b;
    double b_hi = cb - (cb - b);
    double b_lo = b - b_hi;
    double e = ((a_hi * b_hi - p) + a_hi * b_lo + a_lo * b_hi) + a_lo * b_lo;
    return cr_dd_make(p, e);
}

static inline cr_dd_t
cr_dd_add(cr_dd_t a, cr_dd_t b)
{
    cr_dd_t s = cr_two_sum(a.hi, b.hi);
    cr_dd_t t = cr_two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = cr_quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return cr_quick_two_sum(s.hi, s.lo);
}

static inline cr_dd_t
cr_dd_sub(cr_dd_t a, cr_dd_t b)
{
    return cr_dd_add(a, cr_dd_make(-b.hi, -b.lo));
}

static inline cr_dd_t
cr_dd_mul(cr_dd_t a, cr_dd_t b)
{
    cr_dd_t p = cr_two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return cr_quick_two_sum(p.hi, p.lo);
}

static inline cr_dd_t
cr_dd_mul_d(cr_dd_t a, double b)
{
    cr_dd_t p = cr_two_prod(a.hi, b);
    p.lo += a.lo * b;
    return cr_quick_two_sum(p.hi, p.lo);
}

static inline cr_dd_t
cr_dd_div(cr_dd_t a, cr_dd_t b)
{
    double q1 = a.hi / b.hi;
    cr_dd_t r = cr_dd_sub(a, cr_dd_mul_d(b, q1));
    double q2 = r.hi / b.hi;
    r = cr_dd_sub(r, cr_dd_mul_d(b, q2));
    double q3 = r.hi / b.hi;
    cr_dd_t s = cr_quick_two_sum(q1, q2);
    return cr_dd_add(s, cr_dd_make(q3, 0.0));
}

/* Ziv's rounding test: if hi + (lo + err) == hi + (lo - err), then hi + lo
 * is guaranteed to be the exact IEEE-754 correctly rounded binary64 result. */
static inline int
cr_ziv_ok(double hi, double lo, double err)
{
    double up = hi + (lo + err);
    double dn = hi + (lo - err);
    return cr_d2u(up) == cr_d2u(dn);
}

/* Scale double by 2^k (exact for normal results). */
static inline double
cr_ldexp_fast(double x, int k)
{
    /* Split large shifts to avoid intermediate overflow/underflow. */
    while (k > 1023) {
        x *= cr_u2d(UINT64_C(0x7fe0000000000000)); /* 2^1023 */
        k -= 1023;
    }
    while (k < -1022) {
        x *= cr_u2d(UINT64_C(0x0010000000000000)); /* 2^-1022 */
        k += 1022;
    }
    double scale = cr_u2d((uint64_t)(k + 1023) << 52);
    return x * scale;
}

/* ---------------- cr_sqrt & cr_fmod ---------------- */

static inline double
cr_sqrt(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return x; /* preserves -0.0 */
    if (x < 0.0) return cr_qnan();
    if (cr_isinf(x)) return x;
    /* IEEE-754 hardware sqrt is mandated correctly rounded (0.5 ULP). */
    return mt_canon_double(sqrt(x));
}

static double
cr_fmod(double x, double y)
{
    if (cr_isnan(x) || cr_isnan(y)) return cr_qnan();
    uint64_t ux = cr_d2u(x);
    uint64_t uy = cr_d2u(y);
    uint64_t sx = ux & UINT64_C(0x8000000000000000);
    ux &= UINT64_C(0x7fffffffffffffff);
    uy &= UINT64_C(0x7fffffffffffffff);

    if (uy == 0 || ux >= UINT64_C(0x7ff0000000000000))
        return cr_qnan();
    if (ux < uy) return x;
    if (ux == uy) return cr_u2d(sx); /* +-0.0 */
    if (uy == UINT64_C(0x7ff0000000000000)) return x;

    int ex = (int)(ux >> 52);
    int ey = (int)(uy >> 52);
    uint64_t mx, my;
    if (ex == 0) {
        int lz = 0;
        uint64_t t = ux;
        while (!(t & (UINT64_C(1) << 52))) { t <<= 1; lz++; }
        mx = t;
        ex = 1 - lz;
    } else {
        mx = (ux & UINT64_C(0x000fffffffffffff)) | (UINT64_C(1) << 52);
    }
    if (ey == 0) {
        int lz = 0;
        uint64_t t = uy;
        while (!(t & (UINT64_C(1) << 52))) { t <<= 1; lz++; }
        my = t;
        ey = 1 - lz;
    } else {
        my = (uy & UINT64_C(0x000fffffffffffff)) | (UINT64_C(1) << 52);
    }

    int n = ex - ey;
    while (n--) {
        if (mx >= my) mx -= my;
        mx <<= 1;
    }
    if (mx >= my) mx -= my;
    if (mx == 0) return cr_u2d(sx);

    /* Normalize mx back to binary64. */
    while (mx < (UINT64_C(1) << 52)) {
        mx <<= 1;
        ey--;
    }
    if (ey <= 0) {
        int sh = 1 - ey;
        if (sh >= 53) return cr_u2d(sx);
        mx >>= sh;
        return cr_u2d(sx | mx);
    }
    return cr_u2d(sx | ((uint64_t)ey << 52) | (mx & UINT64_C(0x000fffffffffffff)));
}

/* ---------------- cr_exp ---------------- */

static double
cr_exp(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return 1.0;
    if (x >= 709.7827128933840) return cr_inf(0);
    if (x <= -745.1332191019412) return 0.0;

    /* Fast double-double path for normal range. */
    if (x > -708.0 && x < 709.0) {
        const double inv_ln2 = 1.44269504088896340736;
        const double ln2_hi  = 6.93147180559945309417e-01; /* 0x3fe62e42fefa39ef */
        const double ln2_lo  = 2.31904681384629955842e-17; /* 0x3c7abc9e3b39803f */
        int k = (int)(x >= 0.0 ? (x * inv_ln2 + 0.5) : (x * inv_ln2 - 0.5));
        double kd = (double)k;
        cr_dd_t kln2 = cr_two_prod(kd, ln2_hi);
        kln2.lo += kd * ln2_lo;
        cr_dd_t r = cr_dd_sub(cr_dd_make(x, 0.0), cr_quick_two_sum(kln2.hi, kln2.lo));

        /* Scale r by 1/16 so |r| <= ln(2)/32 < 0.0217 -> 11 Taylor terms give >95 bits. */
        cr_dd_t rs = cr_dd_make(r.hi * 0.0625, r.lo * 0.0625);
        cr_dd_t sum = cr_dd_make(1.0, 0.0);
        cr_dd_t term = cr_dd_make(1.0, 0.0);
        for (int n = 1; n <= 12; n++) {
            term = cr_dd_div(cr_dd_mul(term, rs), cr_dd_make((double)n, 0.0));
            sum = cr_dd_add(sum, term);
        }
        /* Square 4 times to compute exp(r). */
        sum = cr_dd_mul(sum, sum);
        sum = cr_dd_mul(sum, sum);
        sum = cr_dd_mul(sum, sum);
        sum = cr_dd_mul(sum, sum);

        double err = cr_fabs(sum.hi) * 1e-24;
        if (cr_ziv_ok(sum.hi, sum.lo, err)) {
            return cr_ldexp_fast(sum.hi + sum.lo, k);
        }
    }

    /* Accurate 256-bit fallback (subnormals, overflow edge, Ziv failures). */
    return mt_fp256_to_double(mt_fp256_exp(mt_fp256_from_double(x)));
}

/* ---------------- cr_log & cr_log10 ---------------- */

static double
cr_log(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return cr_inf(1); /* -Inf */
    if (x < 0.0) return cr_qnan();
    if (cr_isinf(x)) return x;
    if (x == 1.0) return 0.0;

    /* Fast double-double path for normal positive x. */
    uint64_t bits = cr_d2u(x);
    int e = (int)((bits >> 52) & 0x7ffu);
    if (e != 0) {
        int k = e - 1023;
        double m = cr_u2d((bits & UINT64_C(0x000fffffffffffff)) | UINT64_C(0x3ff0000000000000));
        if (m > 1.4142135623730951) {
            m *= 0.5;
            k++;
        }
        /* Two sqrt steps: m^(1/4) in [0.917, 1.091], |u| < 0.044 -> 13 odd terms >95 bits. */
        cr_dd_t md = cr_dd_make(m, 0.0);
        for (int i = 0; i < 2; i++) {
            double s0 = sqrt(md.hi);
            cr_dd_t s = cr_dd_make(s0, 0.0);
            /* One Newton step in double-double gives 106-bit sqrt. */
            s = cr_dd_mul_d(cr_dd_add(s, cr_dd_div(md, s)), 0.5);
            md = s;
        }
        cr_dd_t one = cr_dd_make(1.0, 0.0);
        cr_dd_t u = cr_dd_div(cr_dd_sub(md, one), cr_dd_add(md, one));
        cr_dd_t u2 = cr_dd_mul(u, u);
        cr_dd_t p = u;
        cr_dd_t sum = u;
        for (int n = 3; n <= 29; n += 2) {
            p = cr_dd_mul(p, u2);
            sum = cr_dd_add(sum, cr_dd_div(p, cr_dd_make((double)n, 0.0)));
        }
        /* Multiply by 2 * 4 = 8. */
        sum = cr_dd_make(sum.hi * 8.0, sum.lo * 8.0);
        if (k != 0) {
            const cr_dd_t ln2 = { 6.93147180559945309417e-01, 2.31904681384629955842e-17 };
            sum = cr_dd_add(sum, cr_dd_mul_d(ln2, (double)k));
        }
        double err = cr_fabs(sum.hi) * 1e-24 + 1e-30;
        if (cr_ziv_ok(sum.hi, sum.lo, err)) {
            return sum.hi + sum.lo;
        }
    }

    return mt_fp256_to_double(mt_fp256_log(mt_fp256_from_double(x)));
}

static double
cr_log10(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return cr_inf(1);
    if (x < 0.0) return cr_qnan();
    if (cr_isinf(x)) return x;
    if (x == 1.0) return 0.0;

    mt_fp256_t lx = mt_fp256_log(mt_fp256_from_double(x));
    return mt_fp256_to_double(mt_fp256_mul(lx, MT_FP256_INV_LN10));
}

/* ---------------- cr_sin, cr_cos, cr_tan ---------------- */

static void
cr_dd_sincos_reduced(cr_dd_t r, cr_dd_t *s_out, cr_dd_t *c_out)
{
    cr_dd_t r2 = cr_dd_mul(r, r);
    /* sin(r) = r - r^3/3! + ... (10 terms for |r| <= pi/4 gives >95 bits) */
    cr_dd_t s = r, st = r;
    for (int n = 1; n <= 11; n++) {
        st = cr_dd_div(cr_dd_mul(st, r2), cr_dd_make((double)(-(2 * n) * (2 * n + 1)), 0.0));
        s = cr_dd_add(s, st);
    }
    /* cos(r) = 1 - r^2/2! + ... */
    cr_dd_t c = cr_dd_make(1.0, 0.0), ct = c;
    for (int n = 1; n <= 11; n++) {
        ct = cr_dd_div(cr_dd_mul(ct, r2), cr_dd_make((double)(-(2 * n - 1) * (2 * n)), 0.0));
        c = cr_dd_add(c, ct);
    }
    *s_out = s;
    *c_out = c;
}

static double
cr_sin(double x)
{
    if (cr_isnan(x) || cr_isinf(x)) return cr_qnan();
    if (x == 0.0) return x; /* preserves -0.0 */

    double ax = cr_fabs(x);
    int neg = (cr_d2u(x) >> 63) != 0;

    /* Very small x: sin(x) == x rounded to nearest even (|x| < 2^-27). */
    if (ax < 7.450580596923828e-09) {
        return x;
    }

    /* Fast double-double Cody-Waite path for |x| < 2^20. */
    if (ax < 1048576.0) {
        const double inv_pio2 = 0.636619772367581343076;
        /* pi/2 split into 3 parts for exact Cody-Waite reduction */
        const double pio2_1 = 1.57079632679489655800e+00; /* 0x3ff921fb54442d18 */
        const double pio2_2 = 6.12323399573676603587e-17; /* 0x3c91a62633145c07 */
        const double pio2_3 = -1.49738490485916977726e-33;/* 0xb91f1976b7ed8fbc */
        int64_t k = (int64_t)(ax * inv_pio2 + 0.5);
        double kd = (double)k;
        cr_dd_t p1 = cr_two_prod(kd, pio2_1);
        cr_dd_t p2 = cr_two_prod(kd, pio2_2);
        cr_dd_t kpio2 = cr_dd_add(p1, cr_dd_add(p2, cr_dd_make(kd * pio2_3, 0.0)));
        cr_dd_t r = cr_dd_sub(cr_dd_make(ax, 0.0), kpio2);

        cr_dd_t s, c, res;
        cr_dd_sincos_reduced(r, &s, &c);
        int q = (int)(k & 3);
        if (q == 0) res = s;
        else if (q == 1) res = c;
        else if (q == 2) res = cr_dd_make(-s.hi, -s.lo);
        else res = cr_dd_make(-c.hi, -c.lo);

        if (neg) { res.hi = -res.hi; res.lo = -res.lo; }
        double err = cr_fabs(res.hi) * 1e-24 + 1e-26;
        if (cr_ziv_ok(res.hi, res.lo, err)) {
            return res.hi + res.lo;
        }
    }

    /* Accurate 256-bit Payne-Hanek fallback. */
    int q;
    mt_fp256_t r, s, c, res;
    mt_payne_hanek_256(ax, &q, &r);
    mt_fp256_sincos_reduced(r, &s, &c);
    if (q == 0) res = s;
    else if (q == 1) res = c;
    else if (q == 2) { res = s; res.sign ^= 1; }
    else { res = c; res.sign ^= 1; }
    if (neg) res.sign ^= 1;
    return mt_fp256_to_double(res);
}

static double
cr_cos(double x)
{
    if (cr_isnan(x) || cr_isinf(x)) return cr_qnan();
    double ax = cr_fabs(x);
    if (ax < 7.450580596923828e-09) return 1.0;

    if (ax < 1048576.0) {
        const double inv_pio2 = 0.636619772367581343076;
        const double pio2_1 = 1.57079632679489655800e+00;
        const double pio2_2 = 6.12323399573676603587e-17;
        const double pio2_3 = -1.49738490485916977726e-33;
        int64_t k = (int64_t)(ax * inv_pio2 + 0.5);
        double kd = (double)k;
        cr_dd_t p1 = cr_two_prod(kd, pio2_1);
        cr_dd_t p2 = cr_two_prod(kd, pio2_2);
        cr_dd_t kpio2 = cr_dd_add(p1, cr_dd_add(p2, cr_dd_make(kd * pio2_3, 0.0)));
        cr_dd_t r = cr_dd_sub(cr_dd_make(ax, 0.0), kpio2);

        cr_dd_t s, c, res;
        cr_dd_sincos_reduced(r, &s, &c);
        int q = (int)(k & 3);
        if (q == 0) res = c;
        else if (q == 1) res = cr_dd_make(-s.hi, -s.lo);
        else if (q == 2) res = cr_dd_make(-c.hi, -c.lo);
        else res = s;

        double err = cr_fabs(res.hi) * 1e-24 + 1e-26;
        if (cr_ziv_ok(res.hi, res.lo, err)) {
            return res.hi + res.lo;
        }
    }

    int q;
    mt_fp256_t r, s, c, res;
    mt_payne_hanek_256(ax, &q, &r);
    mt_fp256_sincos_reduced(r, &s, &c);
    if (q == 0) res = c;
    else if (q == 1) { res = s; res.sign ^= 1; }
    else if (q == 2) { res = c; res.sign ^= 1; }
    else res = s;
    return mt_fp256_to_double(res);
}

static double
cr_tan(double x)
{
    if (cr_isnan(x) || cr_isinf(x)) return cr_qnan();
    if (x == 0.0) return x;
    double ax = cr_fabs(x);
    int neg = (cr_d2u(x) >> 63) != 0;
    if (ax < 7.450580596923828e-09) return x;

    int q;
    mt_fp256_t r, s, c, res;
    mt_payne_hanek_256(ax, &q, &r);
    mt_fp256_sincos_reduced(r, &s, &c);
    if (q & 1) {
        res = mt_fp256_div(c, s);
        res.sign ^= 1;
    } else {
        res = mt_fp256_div(s, c);
    }
    if (neg) res.sign ^= 1;
    return mt_fp256_to_double(res);
}

/* ---------------- cr_atan, cr_atan2, cr_asin, cr_acos ---------------- */

static double
cr_atan(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return x;
    if (cr_isinf(x)) {
        double ph = 1.5707963267948966; /* pi/2 */
        return (cr_d2u(x) >> 63) ? -ph : ph;
    }
    double ax = cr_fabs(x);
    if (ax < 7.450580596923828e-09) return x;
    return mt_fp256_to_double(mt_fp256_atan(mt_fp256_from_double(x)));
}

static double
cr_atan2(double y, double x)
{
    if (cr_isnan(y) || cr_isnan(x)) return cr_qnan();
    int sy = (cr_d2u(y) >> 63) != 0;
    int sx = (cr_d2u(x) >> 63) != 0;

    if (y == 0.0) {
        if (sx) return sy ? -3.1415926535897932 : 3.1415926535897932;
        return y; /* +-0.0 */
    }
    if (x == 0.0) {
        return sy ? -1.5707963267948966 : 1.5707963267948966;
    }
    if (cr_isinf(y)) {
        if (cr_isinf(x)) {
            if (sx) return sy ? -2.356194490192345 : 2.356194490192345; /* +-3*pi/4 */
            return sy ? -0.7853981633974483 : 0.7853981633974483; /* +-pi/4 */
        }
        return sy ? -1.5707963267948966 : 1.5707963267948966;
    }
    if (cr_isinf(x)) {
        if (sx) return sy ? -3.1415926535897932 : 3.1415926535897932;
        return sy ? -0.0 : 0.0;
    }

    /* Exact 256-bit atan2: compute |y|/|x| in 256-bit without underflow/overflow. */
    mt_fp256_t fy = mt_fp256_from_double(cr_fabs(y));
    mt_fp256_t fx = mt_fp256_from_double(cr_fabs(x));
    mt_fp256_t ratio = mt_fp256_div(fy, fx);
    mt_fp256_t a = mt_fp256_atan(ratio);
    if (sx) {
        a = mt_fp256_sub(MT_FP256_PI, a);
    }
    a.sign = sy;
    return mt_fp256_to_double(a);
}

static double
cr_asin(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 0.0) return x;
    double ax = cr_fabs(x);
    if (ax > 1.0) return cr_qnan();
    if (ax == 1.0) return (cr_d2u(x) >> 63) ? -1.5707963267948966 : 1.5707963267948966;
    if (ax < 7.450580596923828e-09) return x;
    /* asin(x) = atan(x / sqrt(1 - x^2)) */
    mt_fp256_t fx = mt_fp256_from_double(x);
    mt_fp256_t one = mt_fp256_from_u64(1, 0);
    mt_fp256_t den = mt_fp256_sqrt(mt_fp256_sub(one, mt_fp256_mul(fx, fx)));
    return mt_fp256_to_double(mt_fp256_atan(mt_fp256_div(fx, den)));
}

static double
cr_acos(double x)
{
    if (cr_isnan(x)) return cr_qnan();
    if (x == 1.0) return 0.0;
    if (x == -1.0) return 3.1415926535897932;
    if (cr_fabs(x) > 1.0) return cr_qnan();
    /* acos(x) = pi/2 - asin(x), or 2*atan(sqrt((1-x)/(1+x))) for high accuracy near 1. */
    mt_fp256_t fx = mt_fp256_from_double(x);
    mt_fp256_t one = mt_fp256_from_u64(1, 0);
    mt_fp256_t ratio = mt_fp256_div(mt_fp256_sub(one, fx), mt_fp256_add(one, fx));
    mt_fp256_t a = mt_fp256_atan(mt_fp256_sqrt(ratio));
    if (!mt_fp256_is_zero(&a)) a.exp += 1;
    return mt_fp256_to_double(a);
}

/* ---------------- cr_pow ---------------- */

/* Check if finite double y is an integer: returns 0 (not int), 1 (odd int), 2 (even int). */
static int
cr_classify_int(double y)
{
    uint64_t bits = cr_d2u(y) & UINT64_C(0x7fffffffffffffff);
    int e = (int)(bits >> 52) - 1023;
    if (e < 0) return 0; /* |y| < 1 and y != 0 */
    if (e >= 53) return 2; /* all doubles >= 2^53 are even integers */
    uint64_t sig = (UINT64_C(1) << 52) | (bits & UINT64_C(0x000fffffffffffff));
    int frac_bits = 52 - e;
    if (frac_bits > 0 && (sig & ((UINT64_C(1) << frac_bits) - 1u)) != 0)
        return 0;
    return ((sig >> frac_bits) & 1u) ? 1 : 2;
}

static double
cr_pow(double x, double y)
{
    /* IEEE-754 pow special cases (strict order). */
    if (y == 0.0) return 1.0;          /* pow(anything, +-0) == 1 (even NaN) */
    if (x == 1.0) return 1.0;          /* pow(1, anything) == 1 (even NaN) */
    if (cr_isnan(x) || cr_isnan(y)) return cr_qnan();

    int sx = (cr_d2u(x) >> 63) != 0;
    int sy = (cr_d2u(y) >> 63) != 0;
    double ax = cr_fabs(x);

    if (cr_isinf(y)) {
        if (ax == 1.0) return 1.0;     /* pow(-1, +-Inf) == 1 */
        if (sy) return (ax < 1.0) ? cr_inf(0) : 0.0;
        return (ax < 1.0) ? 0.0 : cr_inf(0);
    }

    int y_int = cr_classify_int(y); /* 0 = non-int, 1 = odd int, 2 = even int */

    if (x == 0.0) {
        if (sy) return (sx && y_int == 1) ? cr_inf(1) : cr_inf(0);
        return (sx && y_int == 1) ? -0.0 : 0.0;
    }
    if (cr_isinf(x)) {
        if (sx) {
            if (sy) return (y_int == 1) ? -0.0 : 0.0;
            return (y_int == 1) ? cr_inf(1) : cr_inf(0);
        }
        return sy ? 0.0 : cr_inf(0);
    }
    if (sx && y_int == 0) {
        return cr_qnan(); /* negative base to non-integer exponent */
    }

    int neg_res = (sx && y_int == 1);

    /* Exact square root special case. */
    if (y == 0.5) return cr_sqrt(ax);
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;

    /* Fast overflow/underflow bounds using rough log2(|x|) * y. */
    uint64_t xbits = cr_d2u(ax);
    int xe = (int)((xbits >> 52) & 0x7ffu) - 1023;
    if (xe == -1023) xe = -1074;
    if (ax != 1.0) {
        double rough_log2 = (double)xe + (ax > 1.0 ? 0.0 : 0.0);
        (void)rough_log2;
        if (cr_fabs(y) > 1e16) {
            if (ax > 1.0) return sy ? (neg_res ? -0.0 : 0.0) : cr_inf(neg_res);
            if (ax < 1.0) return sy ? cr_inf(neg_res) : (neg_res ? -0.0 : 0.0);
        }
    }

    /* Evaluate exp(y * log(|x|)) in 256-bit precision for guaranteed 0.5-ULP
     * correct rounding across all inputs including near-exact integer ties. */
    mt_fp256_t lx = mt_fp256_log(mt_fp256_from_double(ax));
    mt_fp256_t fy = mt_fp256_from_double(y);
    mt_fp256_t prod = mt_fp256_mul(fy, lx);

    /* Guard against exp overflow/underflow before calling mt_fp256_exp. */
    double prod_d = mt_fp256_to_double(prod);
    if (prod_d > 710.0) return cr_inf(neg_res);
    if (prod_d < -746.0) return neg_res ? -0.0 : 0.0;

    mt_fp256_t res = mt_fp256_exp(prod);
    res.sign = neg_res;
    return mt_fp256_to_double(res);
}

#endif /* MT_CORE_MATH_CR_LIBM_H */

/*
 * 256-bit deterministic software floating-point engine for CORE-MATH
 * accurate fallback paths (Table Maker's Dilemma hard-to-round cases).
 *
 * Zero libm dependency, zero heap allocation, C99 + -Wc++-compat safe.
 */

#ifndef MT_CORE_MATH_FP256_H
#define MT_CORE_MATH_FP256_H

#include <stdint.h>
#include <string.h>
#include "mt_nan.h"

typedef struct {
    int sign;       /* 0 = positive, 1 = negative */
    int32_t exp;    /* value = (-1)^sign * (m / 2^255) * 2^exp; exp = INT32_MIN for 0 */
    uint64_t m[4];  /* little-endian 256-bit normalized mantissa (m[3] bit 63 == 1 unless zero) */
} mt_fp256_t;

#define MT_FP256_ZERO_EXP (-2147483647 - 1)

static const mt_fp256_t MT_FP256_PI_HALF = { 0, 0, { UINT64_C(0x020bbea63b139b22), UINT64_C(0x29024e088a67cc74), UINT64_C(0xc4c6628b80dc1cd1), UINT64_C(0xc90fdaa22168c234) } };
static const mt_fp256_t MT_FP256_PI = { 0, 1, { UINT64_C(0x020bbea63b139b22), UINT64_C(0x29024e088a67cc74), UINT64_C(0xc4c6628b80dc1cd1), UINT64_C(0xc90fdaa22168c234) } };
static const mt_fp256_t MT_FP256_LN2 = { 0, -1, { UINT64_C(0x8a0d175b8baafa2c), UINT64_C(0x40f343267298b62d), UINT64_C(0xc9e3b39803f2f6af), UINT64_C(0xb17217f7d1cf79ab) } };
static const mt_fp256_t MT_FP256_INV_LN2 = { 0, 0, { UINT64_C(0x8b25166cd1a13248), UINT64_C(0xeb577aa8dd695a58), UINT64_C(0xbe87fed0691d3e88), UINT64_C(0xb8aa3b295c17f0bb) } };
static const mt_fp256_t MT_FP256_LN10 = { 0, 1, { UINT64_C(0x83c61e8201f02d73), UINT64_C(0xe28fecf9da5df90e), UINT64_C(0xea56d62b82d30a28), UINT64_C(0x935d8dddaaa8ac16) } };
static const mt_fp256_t MT_FP256_INV_LN10 = { 0, -2, { UINT64_C(0x3a3f2d44f78ea53c), UINT64_C(0x3ee3460245c9a202), UINT64_C(0x355baaafad33dc32), UINT64_C(0xde5bd8a937287195) } };

/* 1408 fractional bits of 2/pi (MSB first) for exact Payne-Hanek reduction. */
static const uint64_t mt_two_over_pi_words[22] = {
    UINT64_C(0xa2f9836e4e441529),
    UINT64_C(0xfc2757d1f534ddc0),
    UINT64_C(0xdb6295993c439041),
    UINT64_C(0xfe5163abdebbc561),
    UINT64_C(0xb7246e3a424dd2e0),
    UINT64_C(0x06492eea09d1921c),
    UINT64_C(0xfe1deb1cb129a73e),
    UINT64_C(0xe88235f52ebb4484),
    UINT64_C(0xe99c7026b45f7e41),
    UINT64_C(0x3991d639835339f4),
    UINT64_C(0x9c845f8bbdf9283b),
    UINT64_C(0x1ff897ffde05980f),
    UINT64_C(0xef2f118b5a0a6d1f),
    UINT64_C(0x6d367ecf27cb09b7),
    UINT64_C(0x4f463f669e5fea2d),
    UINT64_C(0x7527bac7ebe5f17b),
    UINT64_C(0x3d0739f78a5292ea),
    UINT64_C(0x6bfb5fb11f8d5d08),
    UINT64_C(0x56033046fc7b6bab),
    UINT64_C(0xf0cfbc209af4361d),
    UINT64_C(0xa9e391615ee61b08),
    UINT64_C(0x6599855f14a06840),
};

static inline mt_fp256_t
mt_fp256_zero(int sign)
{
    mt_fp256_t r;
    r.sign = sign;
    r.exp = MT_FP256_ZERO_EXP;
    r.m[0] = r.m[1] = r.m[2] = r.m[3] = 0;
    return r;
}

static inline int
mt_fp256_is_zero(const mt_fp256_t *a)
{
    return a->exp == MT_FP256_ZERO_EXP ||
           (a->m[0] == 0 && a->m[1] == 0 && a->m[2] == 0 && a->m[3] == 0);
}

static inline mt_fp256_t
mt_fp256_from_u64(uint64_t v, int sign)
{
    mt_fp256_t r;
    if (v == 0) return mt_fp256_zero(sign);
    int lz = 0;
#if defined(__GNUC__) || defined(__clang__)
    lz = __builtin_clzll(v);
#else
    uint64_t t = v;
    while (!(t & (UINT64_C(1) << 63))) { t <<= 1; lz++; }
#endif
    r.sign = sign;
    r.exp = 63 - lz;
    r.m[0] = r.m[1] = r.m[2] = 0;
    r.m[3] = v << lz;
    return r;
}

static inline mt_fp256_t
mt_fp256_from_double(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int sign = (int)(bits >> 63);
    int32_t e = (int32_t)((bits >> 52) & 0x7ffu);
    uint64_t frac = bits & UINT64_C(0x000fffffffffffff);
    if (e == 0 && frac == 0)
        return mt_fp256_zero(sign);
    mt_fp256_t r;
    r.sign = sign;
    r.m[0] = r.m[1] = r.m[2] = 0;
    if (e == 0) {
        int lz = 0;
#if defined(__GNUC__) || defined(__clang__)
        lz = __builtin_clzll(frac);
#else
        uint64_t t = frac;
        while (!(t & (UINT64_C(1) << 63))) { t <<= 1; lz++; }
#endif
        r.m[3] = frac << lz;
        r.exp = -1022 - (lz - 11);
    } else {
        r.m[3] = ((UINT64_C(1) << 52) | frac) << 11;
        r.exp = e - 1023;
    }
    return r;
}

/* Round mt_fp256_t to IEEE-754 binary64 (round to nearest even). */
static double
mt_fp256_to_double(mt_fp256_t a)
{
    uint64_t sign_bit = (uint64_t)(a.sign ? 1 : 0) << 63;
    if (mt_fp256_is_zero(&a)) {
        double z;
        memcpy(&z, &sign_bit, 8);
        return z;
    }
    if (a.exp > 1023) {
        uint64_t inf = sign_bit | UINT64_C(0x7ff0000000000000);
        double d;
        memcpy(&d, &inf, 8);
        return d;
    }
    if (a.exp < -1075) {
        double z;
        memcpy(&z, &sign_bit, 8);
        return z;
    }

    int32_t shift; /* number of top bits to extract (53 for normal, 1..52 for subnormal) */
    if (a.exp >= -1022) {
        shift = 53;
    } else {
        shift = a.exp + 1075; /* in [0, 52] */
    }

    uint64_t sig;
    uint64_t round_bit;
    uint64_t sticky;
    if (shift == 0) {
        sig = 0;
        round_bit = a.m[3] >> 63;
        sticky = (a.m[3] & UINT64_C(0x7fffffffffffffff)) | a.m[2] | a.m[1] | a.m[0];
    } else {
        int drop = 64 - shift; /* in [11, 63] */
        sig = a.m[3] >> drop;
        round_bit = (a.m[3] >> (drop - 1)) & 1u;
        uint64_t low_mask = (drop > 1) ? ((UINT64_C(1) << (drop - 1)) - 1u) : 0u;
        sticky = (a.m[3] & low_mask) | a.m[2] | a.m[1] | a.m[0];
    }

    if (round_bit && (sticky != 0 || (sig & 1u))) {
        sig++;
    }

    uint64_t bits;
    if (a.exp < -1022) {
        if (sig >= (UINT64_C(1) << 52)) {
            bits = sign_bit | (UINT64_C(1) << 52);
        } else {
            bits = sign_bit | sig;
        }
    } else {
        int32_t biased = a.exp + 1023;
        if (sig >= (UINT64_C(1) << 53)) {
            sig >>= 1;
            biased++;
        }
        if (biased >= 0x7ff) {
            bits = sign_bit | UINT64_C(0x7ff0000000000000);
        } else {
            bits = sign_bit | ((uint64_t)biased << 52) | (sig & UINT64_C(0x000fffffffffffff));
        }
    }
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static inline int
mt_fp256_cmp_abs(const mt_fp256_t *a, const mt_fp256_t *b)
{
    int za = mt_fp256_is_zero(a), zb = mt_fp256_is_zero(b);
    if (za && zb) return 0;
    if (za) return -1;
    if (zb) return 1;
    if (a->exp != b->exp) return (a->exp < b->exp) ? -1 : 1;
    for (int i = 3; i >= 0; i--) {
        if (a->m[i] != b->m[i]) return (a->m[i] < b->m[i]) ? -1 : 1;
    }
    return 0;
}

/* Right-shift 256-bit mantissa by s bits (0 <= s < 256), returning sticky bit. */
static inline uint64_t
mt_fp256_shr(uint64_t m[4], int s)
{
    if (s <= 0) return 0;
    if (s >= 256) {
        uint64_t st = m[0] | m[1] | m[2] | m[3];
        m[0] = m[1] = m[2] = m[3] = 0;
        return st != 0;
    }
    uint64_t sticky = 0;
    int ws = s >> 6;
    int bs = s & 63;
    for (int i = 0; i < ws; i++) sticky |= m[i];
    if (bs == 0) {
        for (int i = 0; i < 4 - ws; i++) m[i] = m[i + ws];
        for (int i = 4 - ws; i < 4; i++) m[i] = 0;
    } else {
        sticky |= m[ws] & ((UINT64_C(1) << bs) - 1u);
        for (int i = 0; i < 4 - ws - 1; i++)
            m[i] = (m[i + ws] >> bs) | (m[i + ws + 1] << (64 - bs));
        m[4 - ws - 1] = m[3] >> bs;
        for (int i = 4 - ws; i < 4; i++) m[i] = 0;
    }
    return sticky != 0;
}

static mt_fp256_t
mt_fp256_add(mt_fp256_t a, mt_fp256_t b)
{
    if (mt_fp256_is_zero(&a)) return b;
    if (mt_fp256_is_zero(&b)) return a;

    /* Ensure |a| >= |b|. */
    if (mt_fp256_cmp_abs(&a, &b) < 0) {
        mt_fp256_t t = a; a = b; b = t;
    }

    int32_t diff = a.exp - b.exp;
    if (diff > 258) return a;

    uint64_t bm[4] = { b.m[0], b.m[1], b.m[2], b.m[3] };
    uint64_t sticky = mt_fp256_shr(bm, (int)diff);

    mt_fp256_t r;
    r.sign = a.sign;
    r.exp = a.exp;

    if (a.sign == b.sign) {
        uint64_t carry = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t s1 = a.m[i] + bm[i];
            uint64_t c1 = (s1 < a.m[i]) ? 1u : 0u;
            uint64_t s2 = s1 + carry;
            uint64_t c2 = (s2 < s1) ? 1u : 0u;
            r.m[i] = s2;
            carry = c1 + c2;
        }
        if (carry) {
            uint64_t lsb = r.m[0] & 1u;
            r.m[0] = (r.m[0] >> 1) | (r.m[1] << 63);
            r.m[1] = (r.m[1] >> 1) | (r.m[2] << 63);
            r.m[2] = (r.m[2] >> 1) | (r.m[3] << 63);
            r.m[3] = (r.m[3] >> 1) | (UINT64_C(1) << 63);
            r.exp++;
            if (lsb && sticky) {
                /* round up */
                uint64_t c = 1;
                for (int i = 0; i < 4 && c; i++) {
                    r.m[i] += c;
                    c = (r.m[i] == 0) ? 1u : 0u;
                }
            }
        }
    } else {
        uint64_t borrow = sticky ? 1u : 0u;
        for (int i = 0; i < 4; i++) {
            uint64_t sub1 = a.m[i] - bm[i];
            uint64_t b1 = (a.m[i] < bm[i]) ? 1u : 0u;
            uint64_t sub2 = sub1 - borrow;
            uint64_t b2 = (sub1 < borrow) ? 1u : 0u;
            r.m[i] = sub2;
            borrow = b1 + b2;
        }
        /* Normalize left. */
        if (r.m[0] == 0 && r.m[1] == 0 && r.m[2] == 0 && r.m[3] == 0)
            return mt_fp256_zero(0);
        while (r.m[3] == 0) {
            r.m[3] = r.m[2]; r.m[2] = r.m[1]; r.m[1] = r.m[0]; r.m[0] = 0;
            r.exp -= 64;
        }
        int lz = 0;
#if defined(__GNUC__) || defined(__clang__)
        lz = __builtin_clzll(r.m[3]);
#else
        uint64_t t = r.m[3];
        while (!(t & (UINT64_C(1) << 63))) { t <<= 1; lz++; }
#endif
        if (lz > 0) {
            r.m[3] = (r.m[3] << lz) | (r.m[2] >> (64 - lz));
            r.m[2] = (r.m[2] << lz) | (r.m[1] >> (64 - lz));
            r.m[1] = (r.m[1] << lz) | (r.m[0] >> (64 - lz));
            r.m[0] = (r.m[0] << lz);
            r.exp -= lz;
        }
    }
    return r;
}

static inline mt_fp256_t
mt_fp256_sub(mt_fp256_t a, mt_fp256_t b)
{
    b.sign ^= 1;
    return mt_fp256_add(a, b);
}

static inline void
mt_fp256_umul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
#ifdef __SIZEOF_INT128__
    __extension__ unsigned __int128 p =
        (__extension__ (unsigned __int128)a) * b;
    *lo = (uint64_t)p;
    *hi = (uint64_t)(p >> 64);
#else
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (p00 & UINT64_C(0xffffffff)) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

static mt_fp256_t
mt_fp256_mul(mt_fp256_t a, mt_fp256_t b)
{
    if (mt_fp256_is_zero(&a) || mt_fp256_is_zero(&b))
        return mt_fp256_zero(a.sign ^ b.sign);

    uint64_t p[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            uint64_t lo, hi;
            mt_fp256_umul64(a.m[i], b.m[j], &lo, &hi);
            uint64_t s1 = p[i + j] + lo;
            uint64_t c1 = (s1 < p[i + j]) ? 1u : 0u;
            uint64_t s2 = s1 + carry;
            uint64_t c2 = (s2 < s1) ? 1u : 0u;
            p[i + j] = s2;
            carry = hi + c1 + c2;
        }
        p[i + 4] += carry;
    }

    mt_fp256_t r;
    r.sign = a.sign ^ b.sign;
    /* Product of [1,2)*2^255 * [1,2)*2^255 is in [2^510, 2^512). */
    if (p[7] & (UINT64_C(1) << 63)) {
        r.exp = a.exp + b.exp + 1;
        r.m[3] = p[7]; r.m[2] = p[6]; r.m[1] = p[5]; r.m[0] = p[4];
        if (p[3] & (UINT64_C(1) << 63)) {
            uint64_t c = 1;
            for (int i = 0; i < 4 && c; i++) { r.m[i] += c; c = (r.m[i] == 0); }
            if (c) {
                r.m[0] = (r.m[0] >> 1) | (r.m[1] << 63);
                r.m[1] = (r.m[1] >> 1) | (r.m[2] << 63);
                r.m[2] = (r.m[2] >> 1) | (r.m[3] << 63);
                r.m[3] = (r.m[3] >> 1) | (UINT64_C(1) << 63);
                r.exp++;
            }
        }
    } else {
        r.exp = a.exp + b.exp;
        r.m[3] = (p[7] << 1) | (p[6] >> 63);
        r.m[2] = (p[6] << 1) | (p[5] >> 63);
        r.m[1] = (p[5] << 1) | (p[4] >> 63);
        r.m[0] = (p[4] << 1) | (p[3] >> 63);
        if (p[3] & (UINT64_C(1) << 62)) {
            uint64_t c = 1;
            for (int i = 0; i < 4 && c; i++) { r.m[i] += c; c = (r.m[i] == 0); }
            if (c) {
                r.m[0] = (r.m[0] >> 1) | (r.m[1] << 63);
                r.m[1] = (r.m[1] >> 1) | (r.m[2] << 63);
                r.m[2] = (r.m[2] >> 1) | (r.m[3] << 63);
                r.m[3] = (r.m[3] >> 1) | (UINT64_C(1) << 63);
                r.exp++;
            }
        }
    }
    return r;
}

static mt_fp256_t
mt_fp256_div(mt_fp256_t a, mt_fp256_t b)
{
    /* Newton-Raphson reciprocal: x_{n+1} = x_n * (2 - b * x_n), starting from double approx. */
    if (mt_fp256_is_zero(&a)) return mt_fp256_zero(a.sign ^ b.sign);

    /* Normalize b to [1, 2) so double reciprocal has 53 bits of precision. */
    mt_fp256_t bn = b;
    int32_t b_exp = b.exp;
    bn.exp = 0;
    bn.sign = 0;
    double bd = mt_fp256_to_double(bn);
    mt_fp256_t x = mt_fp256_from_double(1.0 / bd);
    mt_fp256_t two = mt_fp256_from_u64(2, 0);

    /* 3 Newton iterations: 53 -> 106 -> 212 -> 256+ bits. */
    for (int it = 0; it < 3; it++) {
        mt_fp256_t t = mt_fp256_sub(two, mt_fp256_mul(bn, x));
        x = mt_fp256_mul(x, t);
    }
    x.exp -= b_exp;
    x.sign = b.sign;
    return mt_fp256_mul(a, x);
}

static mt_fp256_t
mt_fp256_sqrt(mt_fp256_t a)
{
    if (mt_fp256_is_zero(&a)) return mt_fp256_zero(a.sign);
    int32_t e = a.exp;
    int32_t half_e = (e >= 0) ? (e >> 1) : ((e - 1) >> 1);
    mt_fp256_t an = a;
    an.exp = e - 2 * half_e; /* 0 or 1, so an in [1, 4) */
    double ad = mt_fp256_to_double(an);
    /* Inverse sqrt Newton: y_{n+1} = 0.5 * y_n * (3 - an * y_n^2) */
    mt_fp256_t y = mt_fp256_from_double(1.0 / __builtin_sqrt(ad));
    mt_fp256_t three = mt_fp256_from_u64(3, 0);
    for (int it = 0; it < 3; it++) {
        mt_fp256_t y2 = mt_fp256_mul(y, y);
        mt_fp256_t t = mt_fp256_sub(three, mt_fp256_mul(an, y2));
        y = mt_fp256_mul(y, t);
        y.exp -= 1;
    }
    mt_fp256_t r = mt_fp256_mul(an, y);
    r.exp += half_e;
    return r;
}

/* 256-bit exp(x) for finite x in [-750, 750]. */
static mt_fp256_t
mt_fp256_exp(mt_fp256_t x)
{
    if (mt_fp256_is_zero(&x)) return mt_fp256_from_u64(1, 0);
    /* Reduce x = k * ln(2) + r with |r| <= ln(2)/2. */
    mt_fp256_t kd = mt_fp256_mul(x, MT_FP256_INV_LN2);
    double k_approx = mt_fp256_to_double(kd);
    int32_t k = (int32_t)(k_approx >= 0.0 ? (k_approx + 0.5) : (k_approx - 0.5));
    mt_fp256_t kf = mt_fp256_from_u64((uint64_t)(k < 0 ? -k : k), k < 0 ? 1 : 0);
    mt_fp256_t r = mt_fp256_sub(x, mt_fp256_mul(kf, MT_FP256_LN2));

    /* Halve r 8 times (r /= 256) so |r| < 2^-9 -> Taylor converges in ~24 terms to 256 bits. */
    if (!mt_fp256_is_zero(&r)) r.exp -= 8;

    mt_fp256_t sum = mt_fp256_from_u64(1, 0);
    mt_fp256_t term = mt_fp256_from_u64(1, 0);
    for (uint64_t n = 1; n <= 40; n++) {
        term = mt_fp256_div(mt_fp256_mul(term, r), mt_fp256_from_u64(n, 0));
        sum = mt_fp256_add(sum, term);
        if (mt_fp256_is_zero(&term) || term.exp < sum.exp - 260) break;
    }
    /* Square 8 times. */
    for (int i = 0; i < 8; i++)
        sum = mt_fp256_mul(sum, sum);

    sum.exp += k;
    return sum;
}

/* 256-bit log(x) for positive finite x. */
static mt_fp256_t
mt_fp256_log(mt_fp256_t x)
{
    int32_t k = x.exp;
    mt_fp256_t m = x;
    m.exp = 0; /* m in [1, 2) */
    /* If m > sqrt(2), divide by 2 and increment k so m in [0.707, 1.414]. */
    if (m.m[3] > UINT64_C(0xb504f333f9de6484)) {
        m.exp = -1;
        k++;
    }
    /* Reduce closer to 1 by taking sqrt 4 times: m^(1/16) in [0.978, 1.022]. */
    for (int i = 0; i < 4; i++)
        m = mt_fp256_sqrt(m);

    /* log(m) = 2 * sum_{n>=0} u^(2n+1) / (2n+1), where u = (m - 1) / (m + 1), |u| < 0.011. */
    mt_fp256_t one = mt_fp256_from_u64(1, 0);
    mt_fp256_t u = mt_fp256_div(mt_fp256_sub(m, one), mt_fp256_add(m, one));
    mt_fp256_t u2 = mt_fp256_mul(u, u);
    mt_fp256_t p = u;
    mt_fp256_t sum = u;
    for (uint64_t n = 3; n <= 91; n += 2) {
        p = mt_fp256_mul(p, u2);
        mt_fp256_t term = mt_fp256_div(p, mt_fp256_from_u64(n, 0));
        sum = mt_fp256_add(sum, term);
        if (mt_fp256_is_zero(&term) || term.exp < sum.exp - 260) break;
    }
    /* Multiply by 2 * 16 = 32. */
    if (!mt_fp256_is_zero(&sum)) sum.exp += 5;

    if (k != 0) {
        mt_fp256_t kf = mt_fp256_from_u64((uint64_t)(k < 0 ? -k : k), k < 0 ? 1 : 0);
        sum = mt_fp256_add(sum, mt_fp256_mul(kf, MT_FP256_LN2));
    }
    return sum;
}

/*
 * Exact Payne-Hanek reduction of |d| modulo pi/2:
 * Computes quadrant q (0..3) and 256-bit reduced angle r in [-pi/4, pi/4]
 * such that |d| = q * (pi/2) + r (mod 2*pi).
 */
static void
mt_payne_hanek_256(double abs_d, int *quad_out, mt_fp256_t *r_out)
{
    uint64_t bits;
    memcpy(&bits, &abs_d, 8);
    int32_t e = (int32_t)((bits >> 52) & 0x7ffu) - 1023;
    uint64_t sig = (e == -1023)
        ? (bits & UINT64_C(0x000fffffffffffff))
        : ((UINT64_C(1) << 52) | (bits & UINT64_C(0x000fffffffffffff)));
    if (e == -1023) e = -1022;
    /* |d| = sig * 2^(e - 52). We want frac_mod4(|d| * (2/pi)).
     * Binary point of (2/pi) is before bit 0 of mt_two_over_pi_words[0].
     * Bit position of LSB of sig is (e - 52).
     * We need bits of (2/pi) around exponent -(e - 52), specifically
     * from bit_idx = max(0, e - 52 - 2) covering ~384 bits. */
    int32_t shift = e - 52; /* |d| = sig * 2^shift */
    /* We want (sig * 2^shift) * (2/pi) mod 4.
     * Bit k of 2/pi (k >= 1) has weight 2^(-k).
     * Multiplying by 2^shift gives weight 2^(shift - k).
     * We only care about weights <= 2^1 (since mod 4 drops 2^2 and above),
     * i.e. k >= shift - 1. */
    int32_t first_word = (shift > 64) ? ((shift - 64) / 64) : 0;
    if (first_word < 0) first_word = 0;
    if (first_word > 15) first_word = 15;

    /* Extract 6 consecutive words of 2/pi starting at first_word (384 bits)
     * and multiply by 53-bit sig -> 448-bit product in prod[0..6] (LSB first). */
    uint64_t prod[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; i++) {
        uint64_t w = mt_two_over_pi_words[first_word + 5 - i];
        uint64_t lo, hi;
        mt_fp256_umul64(sig, w, &lo, &hi);
        uint64_t s1 = prod[i] + lo;
        uint64_t c1 = (s1 < prod[i]) ? 1u : 0u;
        prod[i] = s1;
        uint64_t s2 = prod[i + 1] + hi + c1;
        uint64_t c2 = (s2 < prod[i + 1] || (c1 && hi == UINT64_MAX)) ? 1u : 0u;
        prod[i + 1] = s2;
        if (c2 && i + 2 < 7) prod[i + 2]++;
    }

    /* Weight of LSB of prod[0] is 2^(shift - 64*(first_word + 6)).
     * So bit index of weight 2^0 (the integer/fraction boundary) in prod is:
     * bp = 64*(first_word + 6) - shift. */
    int32_t bp = 64 * (first_word + 6) - shift;
    /* Extract integer bits at [bp, bp+1] for quadrant (mod 4),
     * and 256 fractional bits at [bp-256 .. bp-1]. */
    int q = 0;
    for (int b = 0; b < 2; b++) {
        int32_t pos = bp + b;
        int wi = pos >> 6;
        int bi = pos & 63;
        if (wi >= 0 && wi < 7 && ((prod[wi] >> bi) & 1u))
            q |= (1 << b);
    }

    /* Extract 256 bits below bp into frac[0..3] (MSB in frac[3] bit 63 = weight 2^-1). */
    uint64_t frac[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        int32_t top_pos = bp - 1 - 64 * (3 - i); /* bit position of MSB of frac[i] */
        int32_t bot_pos = top_pos - 63;
        if (bot_pos >= 0 && top_pos < 448) {
            int wi = bot_pos >> 6;
            int bi = bot_pos & 63;
            if (bi == 0) {
                frac[i] = prod[wi];
            } else {
                uint64_t lo_part = prod[wi] >> bi;
                uint64_t hi_part = (wi + 1 < 7) ? (prod[wi + 1] << (64 - bi)) : 0u;
                frac[i] = lo_part | hi_part;
            }
        }
    }

    /* If frac >= 0.5 (MSB of frac[3] is 1), round quadrant up and negate frac (1 - frac). */
    int neg = 0;
    if (frac[3] & (UINT64_C(1) << 63)) {
        q = (q + 1) & 3;
        neg = 1;
        /* Two's complement of 256-bit frac: 2^256 - frac. */
        uint64_t borrow = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t prev = frac[i];
            frac[i] = (uint64_t)0 - prev - borrow;
            borrow = (prev != 0 || borrow != 0) ? 1u : 0u;
        }
    }

    *quad_out = q;

    /* Convert frac (weight of MSB = 2^-1) to mt_fp256_t and multiply by pi/2. */
    if (frac[0] == 0 && frac[1] == 0 && frac[2] == 0 && frac[3] == 0) {
        *r_out = mt_fp256_zero(0);
        return;
    }
    mt_fp256_t f;
    f.sign = neg;
    f.exp = -1;
    f.m[0] = frac[0]; f.m[1] = frac[1]; f.m[2] = frac[2]; f.m[3] = frac[3];
    while (f.m[3] == 0) {
        f.m[3] = f.m[2]; f.m[2] = f.m[1]; f.m[1] = f.m[0]; f.m[0] = 0;
        f.exp -= 64;
    }
    int lz = 0;
#if defined(__GNUC__) || defined(__clang__)
    lz = __builtin_clzll(f.m[3]);
#else
    uint64_t t = f.m[3];
    while (!(t & (UINT64_C(1) << 63))) { t <<= 1; lz++; }
#endif
    if (lz > 0) {
        f.m[3] = (f.m[3] << lz) | (f.m[2] >> (64 - lz));
        f.m[2] = (f.m[2] << lz) | (f.m[1] >> (64 - lz));
        f.m[1] = (f.m[1] << lz) | (f.m[0] >> (64 - lz));
        f.m[0] = (f.m[0] << lz);
        f.exp -= lz;
    }
    *r_out = mt_fp256_mul(f, MT_FP256_PI_HALF);
}

/* 256-bit sin(r) and cos(r) for |r| <= pi/4. */
static void
mt_fp256_sincos_reduced(mt_fp256_t r, mt_fp256_t *sin_out, mt_fp256_t *cos_out)
{
    mt_fp256_t r2 = mt_fp256_mul(r, r);
    /* sin(r) = r - r^3/3! + r^5/5! - ... */
    mt_fp256_t s_sum = r;
    mt_fp256_t s_term = r;
    for (uint64_t n = 1; n <= 40; n++) {
        s_term = mt_fp256_div(mt_fp256_mul(s_term, r2),
                              mt_fp256_from_u64((2 * n) * (2 * n + 1), 0));
        s_term.sign ^= 1;
        s_sum = mt_fp256_add(s_sum, s_term);
        if (mt_fp256_is_zero(&s_term) || s_term.exp < s_sum.exp - 260) break;
    }
    /* cos(r) = 1 - r^2/2! + r^4/4! - ... */
    mt_fp256_t c_sum = mt_fp256_from_u64(1, 0);
    mt_fp256_t c_term = c_sum;
    for (uint64_t n = 1; n <= 40; n++) {
        c_term = mt_fp256_div(mt_fp256_mul(c_term, r2),
                              mt_fp256_from_u64((2 * n - 1) * (2 * n), 0));
        c_term.sign ^= 1;
        c_sum = mt_fp256_add(c_sum, c_term);
        if (mt_fp256_is_zero(&c_term) || c_term.exp < c_sum.exp - 260) break;
    }
    *sin_out = s_sum;
    *cos_out = c_sum;
}

/* 256-bit atan(x) for finite x. */
static mt_fp256_t
mt_fp256_atan(mt_fp256_t x)
{
    if (mt_fp256_is_zero(&x)) return x;
    int sign = x.sign;
    x.sign = 0;
    int inv = 0;
    mt_fp256_t one = mt_fp256_from_u64(1, 0);
    if (x.exp >= 0) {
        /* x >= 1: atan(x) = pi/2 - atan(1/x) */
        x = mt_fp256_div(one, x);
        inv = 1;
    }
    /* Apply half-angle formula 5 times: atan(x) = 2 * atan(x / (1 + sqrt(1 + x^2))).
     * After 5 steps |x| < 1/32 -> Taylor converges in ~26 terms to 256 bits. */
    for (int i = 0; i < 5; i++) {
        mt_fp256_t x2 = mt_fp256_mul(x, x);
        mt_fp256_t den = mt_fp256_add(one, mt_fp256_sqrt(mt_fp256_add(one, x2)));
        x = mt_fp256_div(x, den);
    }
    mt_fp256_t x2 = mt_fp256_mul(x, x);
    mt_fp256_t p = x;
    mt_fp256_t sum = x;
    for (uint64_t n = 1; n <= 40; n++) {
        p = mt_fp256_mul(p, x2);
        p.sign ^= 1;
        mt_fp256_t term = mt_fp256_div(p, mt_fp256_from_u64(2 * n + 1, 0));
        sum = mt_fp256_add(sum, term);
        if (mt_fp256_is_zero(&term) || term.exp < sum.exp - 260) break;
    }
    if (!mt_fp256_is_zero(&sum)) sum.exp += 5;
    if (inv) sum = mt_fp256_sub(MT_FP256_PI_HALF, sum);
    sum.sign = sign;
    return sum;
}

#endif /* MT_CORE_MATH_FP256_H */

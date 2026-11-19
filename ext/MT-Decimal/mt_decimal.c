/*
 * IEEE 754-2008 decimal128 implementation for MT::Decimal.
 *
 * Self-contained C99, zero heap allocation, threadsafe.
 */

#include "mt_decimal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#if defined(__DEC128_MANT_DIG__) && defined(__powerpc__) && defined(__DFP__)
#  define MT_DEC_USE_HARD_DFP 1
#else
#  define MT_DEC_USE_HARD_DFP 0
#endif

const char *
mt_dec_backend(void)
{
#if MT_DEC_USE_HARD_DFP
    return "dfp";
#else
    return "bid128";
#endif
}

/* ---------------- Constructors for special values ---------------- */

static mt_dec128_t
dec_nan(void)
{
    mt_dec128_t r;
    r.lo = 0; r.hi = 0; r.exp = 0; r.sign = 0; r.kind = MT_DEC_FLAG_NAN; r.pad = 0;
    return r;
}

static mt_dec128_t
dec_inf(uint8_t sign)
{
    mt_dec128_t r;
    r.lo = 0; r.hi = 0; r.exp = 0; r.sign = sign ? 1 : 0; r.kind = MT_DEC_FLAG_INF; r.pad = 0;
    return r;
}

static mt_dec128_t
dec_zero(uint8_t sign, int32_t exp)
{
    mt_dec128_t r;
    if (exp < MT_DEC_ETINY) exp = MT_DEC_ETINY;
    if (exp > MT_DEC_EMAX - MT_DEC_PREC + 1) exp = MT_DEC_EMAX - MT_DEC_PREC + 1;
    r.lo = 0; r.hi = 0; r.exp = exp; r.sign = sign ? 1 : 0; r.kind = MT_DEC_FLAG_FINITE; r.pad = 0;
    return r;
}

static int
dec_is_zero(mt_dec128_t a)
{
    return a.kind == MT_DEC_FLAG_FINITE && a.lo == 0 && a.hi == 0;
}

/* ---------------- 128-bit <-> decimal digits helpers ---------------- */

static void
u128_mul_add_u32(uint64_t *hi, uint64_t *lo, uint32_t mul, uint32_t add)
{
    /* Compute (*hi * 2^64 + *lo) * mul + add */
    uint64_t lo_lo = (*lo & UINT64_C(0xffffffff)) * mul + add;
    uint64_t lo_hi = (*lo >> 32) * mul + (lo_lo >> 32);
    *lo = (lo_lo & UINT64_C(0xffffffff)) | (lo_hi << 32);
    uint64_t carry = lo_hi >> 32;
    *hi = (*hi) * mul + carry;
}

static uint32_t
u128_divmod_u32(uint64_t *hi, uint64_t *lo, uint32_t div)
{
    uint64_t h = *hi;
    uint64_t qh = h / div;
    uint64_t rem = h % div;
    uint64_t l_hi = (rem << 32) | (*lo >> 32);
    uint64_t ql_hi = l_hi / div;
    rem = l_hi % div;
    uint64_t l_lo = (rem << 32) | (*lo & UINT64_C(0xffffffff));
    uint64_t ql_lo = l_lo / div;
    rem = l_lo % div;
    *hi = qh;
    *lo = (ql_hi << 32) | ql_lo;
    return (uint32_t)rem;
}

/* Unpack (hi, lo) into decimal digits[0..len-1] (MSD first). Returns len (0 for 0). */
static int
dec_unpack(mt_dec128_t a, uint8_t digits[40])
{
    uint64_t hi = a.hi, lo = a.lo;
    uint8_t rev[40];
    int n = 0;
    while (hi != 0 || lo != 0) {
        uint32_t r = u128_divmod_u32(&hi, &lo, 10u);
        rev[n++] = (uint8_t)r;
    }
    for (int i = 0; i < n; i++)
        digits[i] = rev[n - 1 - i];
    return n;
}

/* Pack MSD-first digits[0..n-1] (n <= 34) into mt_dec128_t. */
static mt_dec128_t
dec_pack_digits(uint8_t sign, const uint8_t *digits, int n, int32_t exp)
{
    int start = 0;
    while (start < n && digits[start] == 0) start++;
    if (start == n)
        return dec_zero(sign, exp);

    uint64_t hi = 0, lo = 0;
    for (int i = start; i < n; i++)
        u128_mul_add_u32(&hi, &lo, 10u, digits[i]);

    mt_dec128_t r;
    r.lo = lo;
    r.hi = hi;
    r.exp = exp;
    r.sign = sign ? 1 : 0;
    r.kind = MT_DEC_FLAG_FINITE;
    r.pad = 0;
    return r;
}

/* Decide whether to round magnitude up given:
 *   sign: 0 (+), 1 (-)
 *   last_kept: LSB decimal digit (0..9)
 *   first_dropped: first dropped digit (0..9)
 *   has_sticky: non-zero if any digits after first_dropped are non-zero
 *   rmode: MT_DEC_ROUND_*
 */
static int
dec_should_round_up(uint8_t sign, uint8_t last_kept, uint8_t first_dropped,
                    int has_sticky, int rmode)
{
    if (first_dropped == 0 && !has_sticky)
        return 0;
    switch (rmode) {
    case MT_DEC_ROUND_DOWN:
        return 0;
    case MT_DEC_ROUND_FLOOR:
        return sign ? 1 : 0;
    case MT_DEC_ROUND_CEIL:
        return sign ? 0 : 1;
    case MT_DEC_ROUND_HALF_UP:
        return first_dropped >= 5;
    case MT_DEC_ROUND_HALF_EVEN:
    default:
        if (first_dropped > 5) return 1;
        if (first_dropped < 5) return 0;
        if (has_sticky) return 1;
        return (last_kept & 1) != 0;
    }
}

/* Round arbitrary MSD-first digit buffer digits[0..n-1] * 10^exp (with optional
 * extra sticky bit from truncated tail) to IEEE 754-2008 decimal128. */
static mt_dec128_t
dec_finalize(uint8_t sign, const uint8_t *digits_in, int n, int32_t exp,
             int extra_sticky, int rmode)
{
    /* Strip leading zeros. */
    int start = 0;
    while (start < n && digits_in[start] == 0) start++;
    if (start == n) {
        if (extra_sticky) {
            /* Tiny non-zero value below all represented digits. */
            if (dec_should_round_up(sign, 0, 0, 1, rmode)) {
                uint8_t one = 1;
                return dec_pack_digits(sign, &one, 1, MT_DEC_ETINY);
            }
        }
        return dec_zero(sign, exp);
    }

    int len = n - start;
    const uint8_t *d = digits_in + start;

    /* Clamp subnormal exponent: if exp < MT_DEC_ETINY, we must drop (MT_DEC_ETINY - exp) digits. */
    int drop = 0;
    if (len > MT_DEC_PREC)
        drop = len - MT_DEC_PREC;
    if (exp + drop < MT_DEC_ETINY)
        drop = MT_DEC_ETINY - exp;

    uint8_t kept[40];
    int klen;
    uint8_t first_dropped = 0;
    int sticky = extra_sticky;

    if (drop > 0) {
        if (drop > len) {
            /* All digits dropped. */
            if (drop == len + 1) {
                first_dropped = d[0];
                for (int i = 1; i < len; i++) if (d[i]) { sticky = 1; break; }
            } else {
                first_dropped = 0;
                sticky = 1;
            }
            klen = 0;
            exp += drop;
        } else {
            klen = len - drop;
            memcpy(kept, d, (size_t)klen);
            first_dropped = d[klen];
            for (int i = klen + 1; i < len; i++) {
                if (d[i] != 0) { sticky = 1; break; }
            }
            exp += drop;
        }

        uint8_t last_kept = (klen > 0) ? kept[klen - 1] : 0;
        if (dec_should_round_up(sign, last_kept, first_dropped, sticky, rmode)) {
            if (klen == 0) {
                kept[0] = 1;
                klen = 1;
            } else {
                int carry = 1;
                for (int i = klen - 1; i >= 0 && carry; i--) {
                    int s = kept[i] + carry;
                    if (s == 10) { kept[i] = 0; carry = 1; }
                    else { kept[i] = (uint8_t)s; carry = 0; }
                }
                if (carry) {
                    if (klen < MT_DEC_PREC) {
                        memmove(kept + 1, kept, (size_t)klen);
                        kept[0] = 1;
                        klen++;
                    } else {
                        /* 999...9 + 1 -> 100...0 * 10^(exp+1) */
                        kept[0] = 1;
                        memset(kept + 1, 0, (size_t)(MT_DEC_PREC - 1));
                        klen = MT_DEC_PREC;
                        exp++;
                    }
                }
            }
        }
    } else {
        klen = len;
        memcpy(kept, d, (size_t)klen);
    }

    /* Check overflow: adjusted exponent = exp + klen - 1 > MT_DEC_EMAX. */
    int32_t max_q = MT_DEC_EMAX - MT_DEC_PREC + 1; /* 6111 */
    if (klen > 0 && exp + klen - 1 > MT_DEC_EMAX) {
        /* IEEE 754 overflow handling according to rounding mode. */
        if (rmode == MT_DEC_ROUND_DOWN ||
            (rmode == MT_DEC_ROUND_FLOOR && !sign) ||
            (rmode == MT_DEC_ROUND_CEIL && sign)) {
            memset(kept, 9, MT_DEC_PREC);
            return dec_pack_digits(sign, kept, MT_DEC_PREC, max_q);
        }
        return dec_inf(sign);
    }

    /* Clamp exponent <= max_q by appending trailing zeros if needed. */
    if (klen > 0 && exp > max_q) {
        int32_t shift = exp - max_q;
        if (klen + shift <= MT_DEC_PREC) {
            memset(kept + klen, 0, (size_t)shift);
            klen += (int)shift;
            exp = max_q;
        }
    }

    return dec_pack_digits(sign, kept, klen, exp);
}

/* ---------------- Parsing & Constructors ---------------- */

mt_dec128_t
mt_dec_from_i64(int64_t v)
{
    mt_dec128_t r;
    r.exp = 0;
    r.kind = MT_DEC_FLAG_FINITE;
    r.pad = 0;
    r.hi = 0;
    if (v < 0) {
        r.sign = 1;
        r.lo = (uint64_t)(-(v + 1)) + 1u;
    } else {
        r.sign = 0;
        r.lo = (uint64_t)v;
    }
    return r;
}

mt_dec128_t
mt_dec_from_string(const char *s, size_t len, int rmode)
{
    const char *p = s;
    const char *end = s + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    if (p == end) return dec_nan();

    uint8_t sign = 0;
    if (*p == '-') { sign = 1; p++; }
    else if (*p == '+') { p++; }

    size_t rem = (size_t)(end - p);
    if (rem >= 3) {
        if ((p[0] == 'n' || p[0] == 'N') &&
            (p[1] == 'a' || p[1] == 'A') &&
            (p[2] == 'n' || p[2] == 'N')) {
            return dec_nan();
        }
        if ((p[0] == 'i' || p[0] == 'I') &&
            (p[1] == 'n' || p[1] == 'N') &&
            (p[2] == 'f' || p[2] == 'F')) {
            if (rem == 3 ||
                (rem == 8 &&
                 (p[3] == 'i' || p[3] == 'I') &&
                 (p[4] == 'n' || p[4] == 'N') &&
                 (p[5] == 'i' || p[5] == 'I') &&
                 (p[6] == 't' || p[6] == 'T') &&
                 (p[7] == 'y' || p[7] == 'Y'))) {
                return dec_inf(sign);
            }
        }
    }

    uint8_t digits[80];
    int nd = 0;
    int seen_digit = 0;
    int seen_dot = 0;
    int sticky = 0;
    int64_t exp = 0;

    while (p < end) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            seen_digit = 1;
            uint8_t d = (uint8_t)(c - '0');
            if (nd == 0 && d == 0) {
                if (seen_dot) exp--;
            } else if (nd < 72) {
                digits[nd++] = d;
                if (seen_dot) exp--;
            } else {
                if (d != 0) sticky = 1;
                if (!seen_dot) exp++;
            }
            p++;
        } else if (c == '.' && !seen_dot) {
            seen_dot = 1;
            p++;
        } else if (c == '_') {
            p++;
        } else {
            break;
        }
    }

    if (!seen_digit) return dec_nan();

    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        int eneg = 0;
        if (p < end && (*p == '-' || *p == '+')) {
            eneg = (*p == '-');
            p++;
        }
        if (p == end || !(*p >= '0' && *p <= '9')) return dec_nan();
        int64_t e = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (e < 1000000) e = e * 10 + (*p - '0');
            p++;
        }
        exp += eneg ? -e : e;
    }

    if (p != end) return dec_nan();

    if (nd == 0) {
        if (exp < MT_DEC_ETINY) exp = MT_DEC_ETINY;
        if (exp > MT_DEC_EMAX - MT_DEC_PREC + 1) exp = MT_DEC_EMAX - MT_DEC_PREC + 1;
        return dec_zero(sign, (int32_t)exp);
    }

    if (exp > 100000) return dec_inf(sign);
    if (exp < -100000) {
        if (dec_should_round_up(sign, 0, 0, 1, rmode)) {
            uint8_t one = 1;
            return dec_pack_digits(sign, &one, 1, MT_DEC_ETINY);
        }
        return dec_zero(sign, MT_DEC_ETINY);
    }

    return dec_finalize(sign, digits, nd, (int32_t)exp, sticky, rmode);
}

mt_dec128_t
mt_dec_from_double(double d, int rmode)
{
    if (d != d) return dec_nan();
    if (isinf(d)) return dec_inf(d < 0.0 ? 1 : 0);
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.17g", d);
    if (n <= 0) return dec_nan();
    return mt_dec_from_string(buf, (size_t)n, rmode);
}

/* ---------------- Comparison ---------------- */

int
mt_dec_cmp(mt_dec128_t a, mt_dec128_t b)
{
    if (a.kind == MT_DEC_FLAG_NAN || b.kind == MT_DEC_FLAG_NAN)
        return 2;
    if (a.kind == MT_DEC_FLAG_INF || b.kind == MT_DEC_FLAG_INF) {
        if (a.kind == MT_DEC_FLAG_INF && b.kind == MT_DEC_FLAG_INF) {
            if (a.sign == b.sign) return 0;
            return a.sign ? -1 : 1;
        }
        if (a.kind == MT_DEC_FLAG_INF) return a.sign ? -1 : 1;
        return b.sign ? 1 : -1;
    }
    int za = dec_is_zero(a), zb = dec_is_zero(b);
    if (za && zb) return 0;
    if (za) return b.sign ? 1 : -1;
    if (zb) return a.sign ? -1 : 1;
    if (a.sign != b.sign) return a.sign ? -1 : 1;

    uint8_t da[40], db[40];
    int la = dec_unpack(a, da);
    int lb = dec_unpack(b, db);
    int32_t ea = a.exp + la - 1;
    int32_t eb = b.exp + lb - 1;
    int mag = 0;
    if (ea != eb) {
        mag = (ea < eb) ? -1 : 1;
    } else {
        int m = (la > lb) ? la : lb;
        for (int i = 0; i < m; i++) {
            uint8_t va = (i < la) ? da[i] : 0;
            uint8_t vb = (i < lb) ? db[i] : 0;
            if (va != vb) {
                mag = (va < vb) ? -1 : 1;
                break;
            }
        }
    }
    return a.sign ? -mag : mag;
}

/* ---------------- Arithmetic ---------------- */

mt_dec128_t
mt_dec_neg(mt_dec128_t a)
{
    if (a.kind == MT_DEC_FLAG_NAN) return dec_nan();
    a.sign ^= 1;
    return a;
}

mt_dec128_t
mt_dec_abs(mt_dec128_t a)
{
    if (a.kind == MT_DEC_FLAG_NAN) return dec_nan();
    a.sign = 0;
    return a;
}

mt_dec128_t
mt_dec_add(mt_dec128_t a, mt_dec128_t b, int rmode)
{
    if (a.kind == MT_DEC_FLAG_NAN || b.kind == MT_DEC_FLAG_NAN)
        return dec_nan();
    if (a.kind == MT_DEC_FLAG_INF || b.kind == MT_DEC_FLAG_INF) {
        if (a.kind == MT_DEC_FLAG_INF && b.kind == MT_DEC_FLAG_INF) {
            if (a.sign != b.sign) return dec_nan();
            return a;
        }
        return (a.kind == MT_DEC_FLAG_INF) ? a : b;
    }

    int za = dec_is_zero(a), zb = dec_is_zero(b);
    int32_t min_exp = (a.exp < b.exp) ? a.exp : b.exp;
    if (za && zb) {
        uint8_t zsign = (a.sign && b.sign) || (a.sign != b.sign && rmode == MT_DEC_ROUND_FLOOR);
        return dec_zero(zsign, min_exp);
    }
    if (za) {
        /* Preserve preferred quantum min(a.exp, b.exp) when lossless. */
        return b;
    }
    if (zb) {
        return a;
    }

    /* Ensure |a| >= |b| by adjusted magnitude. */
    mt_dec128_t aa = a; aa.sign = 0;
    mt_dec128_t bb = b; bb.sign = 0;
    if (mt_dec_cmp(aa, bb) < 0) {
        mt_dec128_t t = a; a = b; b = t;
    }

    uint8_t da[40], db[40];
    int la = dec_unpack(a, da);
    int lb = dec_unpack(b, db);

    /* If operands are separated by more than 36 digits, b only contributes sticky. */
    int32_t a_top = a.exp + la - 1;
    int32_t b_top = b.exp + lb - 1;
    if (a_top - b_top > MT_DEC_PREC + 2) {
        if (a.sign == b.sign) {
            return dec_finalize(a.sign, da, la, a.exp, 1, rmode);
        } else {
            /* |a| - tiny: subtract 1 ulp at precision+2 if needed. */
            uint8_t buf[44];
            memset(buf, 0, sizeof(buf));
            memcpy(buf, da, (size_t)la);
            int ext = MT_DEC_PREC + 2;
            int shift = (ext > la) ? (ext - la) : 0;
            int total = la + shift;
            int32_t res_exp = a.exp - shift;
            /* Subtract 1 from LSB to represent borrow from sticky b. */
            int borrow = 1;
            for (int i = total - 1; i >= 0 && borrow; i--) {
                if (buf[i] > 0) { buf[i]--; borrow = 0; }
                else { buf[i] = 9; }
            }
            return dec_finalize(a.sign, buf, total, res_exp, 1, rmode);
        }
    }

    /* Align both operands to common exponent cexp = min(a.exp, b.exp),
     * capped so total width <= 80 digits. */
    int32_t cexp = (a.exp < b.exp) ? a.exp : b.exp;
    int32_t top = (a_top > b_top) ? a_top : b_top;
    int width = (int)(top - cexp + 1);
    int extra_sticky = 0;
    if (width > 76) {
        int32_t new_cexp = top - 75;
        /* Check if any dropped digits of b are non-zero. */
        int32_t b_drop = new_cexp - b.exp;
        if (b_drop > 0) {
            for (int i = lb - 1; i >= 0 && (lb - 1 - i) < b_drop; i--) {
                if (db[i] != 0) { extra_sticky = 1; break; }
            }
        }
        cexp = new_cexp;
        width = 76;
    }

    uint8_t va[80], vb[80], res[80];
    memset(va, 0, sizeof(va));
    memset(vb, 0, sizeof(vb));
    memset(res, 0, sizeof(res));

    /* Place digits right-aligned at index width-1 (corresponding to 10^cexp). */
    for (int i = 0; i < la; i++) {
        int pos = width - 1 - (int)((a.exp - cexp) + (la - 1 - i));
        if (pos >= 0 && pos < width) va[pos] = da[i];
    }
    for (int i = 0; i < lb; i++) {
        int pos = width - 1 - (int)((b.exp - cexp) + (lb - 1 - i));
        if (pos >= 0 && pos < width) vb[pos] = db[i];
    }

    if (a.sign == b.sign) {
        int carry = 0;
        for (int i = width - 1; i >= 0; i--) {
            int s = va[i] + vb[i] + carry;
            res[i + 1] = (uint8_t)(s % 10);
            carry = s / 10;
        }
        res[0] = (uint8_t)carry;
        return dec_finalize(a.sign, res, width + 1, cexp, extra_sticky, rmode);
    } else {
        int borrow = extra_sticky ? 1 : 0;
        for (int i = width - 1; i >= 0; i--) {
            int d = (int)va[i] - (int)vb[i] - borrow;
            if (d < 0) { d += 10; borrow = 1; }
            else { borrow = 0; }
            res[i] = (uint8_t)d;
        }
        int all_zero = 1;
        for (int i = 0; i < width; i++) if (res[i] != 0) { all_zero = 0; break; }
        if (all_zero && !extra_sticky) {
            return dec_zero((rmode == MT_DEC_ROUND_FLOOR) ? 1 : 0, min_exp);
        }
        return dec_finalize(a.sign, res, width, cexp, extra_sticky, rmode);
    }
}

mt_dec128_t
mt_dec_sub(mt_dec128_t a, mt_dec128_t b, int rmode)
{
    if (b.kind != MT_DEC_FLAG_NAN) b.sign ^= 1;
    return mt_dec_add(a, b, rmode);
}

mt_dec128_t
mt_dec_mul(mt_dec128_t a, mt_dec128_t b, int rmode)
{
    if (a.kind == MT_DEC_FLAG_NAN || b.kind == MT_DEC_FLAG_NAN)
        return dec_nan();
    uint8_t sign = a.sign ^ b.sign;
    if (a.kind == MT_DEC_FLAG_INF || b.kind == MT_DEC_FLAG_INF) {
        if (dec_is_zero(a) || dec_is_zero(b)) return dec_nan();
        return dec_inf(sign);
    }
    if (dec_is_zero(a) || dec_is_zero(b)) {
        int64_t e = (int64_t)a.exp + (int64_t)b.exp;
        if (e < MT_DEC_ETINY) e = MT_DEC_ETINY;
        if (e > MT_DEC_EMAX - MT_DEC_PREC + 1) e = MT_DEC_EMAX - MT_DEC_PREC + 1;
        return dec_zero(sign, (int32_t)e);
    }

    uint8_t da[40], db[40];
    int la = dec_unpack(a, da);
    int lb = dec_unpack(b, db);
    uint8_t prod[80];
    memset(prod, 0, sizeof(prod));

    for (int i = la - 1; i >= 0; i--) {
        int carry = 0;
        for (int j = lb - 1; j >= 0; j--) {
            int s = prod[i + j + 1] + da[i] * db[j] + carry;
            prod[i + j + 1] = (uint8_t)(s % 10);
            carry = s / 10;
        }
        prod[i] += (uint8_t)carry;
    }

    int64_t rexp = (int64_t)a.exp + (int64_t)b.exp;
    if (rexp > 100000) return dec_inf(sign);
    if (rexp < -100000) {
        if (dec_should_round_up(sign, 0, 0, 1, rmode)) {
            uint8_t one = 1;
            return dec_pack_digits(sign, &one, 1, MT_DEC_ETINY);
        }
        return dec_zero(sign, MT_DEC_ETINY);
    }
    return dec_finalize(sign, prod, la + lb, (int32_t)rexp, 0, rmode);
}

/* Compare two equal-length big-endian digit arrays. */
static int
digits_cmp(const uint8_t *a, int la, const uint8_t *b, int lb)
{
    int ia = 0, ib = 0;
    while (ia < la && a[ia] == 0) ia++;
    while (ib < lb && b[ib] == 0) ib++;
    int na = la - ia, nb = lb - ib;
    if (na != nb) return (na < nb) ? -1 : 1;
    for (int i = 0; i < na; i++) {
        if (a[ia + i] != b[ib + i])
            return (a[ia + i] < b[ib + i]) ? -1 : 1;
    }
    return 0;
}

/* Subtract b[0..lb-1] from a[0..la-1] in place (assumes a >= b). */
static void
digits_sub_inplace(uint8_t *a, int la, const uint8_t *b, int lb)
{
    int borrow = 0;
    for (int i = 0; i < la; i++) {
        int ia = la - 1 - i;
        int ib = lb - 1 - i;
        int bv = (ib >= 0) ? b[ib] : 0;
        int d = (int)a[ia] - bv - borrow;
        if (d < 0) { d += 10; borrow = 1; }
        else { borrow = 0; }
        a[ia] = (uint8_t)d;
    }
}

mt_dec128_t
mt_dec_div(mt_dec128_t a, mt_dec128_t b, int rmode)
{
    if (a.kind == MT_DEC_FLAG_NAN || b.kind == MT_DEC_FLAG_NAN)
        return dec_nan();
    uint8_t sign = a.sign ^ b.sign;
    if (a.kind == MT_DEC_FLAG_INF) {
        if (b.kind == MT_DEC_FLAG_INF) return dec_nan();
        return dec_inf(sign);
    }
    if (b.kind == MT_DEC_FLAG_INF) {
        return dec_zero(sign, 0);
    }
    if (dec_is_zero(b)) {
        if (dec_is_zero(a)) return dec_nan();
        return dec_inf(sign);
    }
    if (dec_is_zero(a)) {
        int64_t e = (int64_t)a.exp - (int64_t)b.exp;
        if (e < MT_DEC_ETINY) e = MT_DEC_ETINY;
        if (e > MT_DEC_EMAX - MT_DEC_PREC + 1) e = MT_DEC_EMAX - MT_DEC_PREC + 1;
        return dec_zero(sign, (int32_t)e);
    }

    uint8_t da[40], db[40];
    int la = dec_unpack(a, da);
    int lb = dec_unpack(b, db);

    /* Align rem so rem >= db (and rem < 10 * db) to start generating quotient digits. */
    uint8_t rem[80];
    memset(rem, 0, sizeof(rem));
    int rlen = la;
    memcpy(rem, da, (size_t)la);

    int64_t qexp = (int64_t)a.exp - (int64_t)b.exp;
    while (digits_cmp(rem, rlen, db, lb) < 0) {
        rem[rlen++] = 0;
        qexp--;
    }

    /* Generate up to 36 quotient digits (34 + guard + round). */
    uint8_t q[40];
    int qlen = 0;
    int max_q = MT_DEC_PREC + 2;

    /* Adjust if rem >= 10 * db by shifting db logically (count extra leading digits). */
    int extra_scale = 0;
    {
        /* Find largest power of 10 shift s such that rem >= db * 10^s. */
        uint8_t tmp_b[80];
        memcpy(tmp_b, db, (size_t)lb);
        while (digits_cmp(rem, rlen, tmp_b, lb + extra_scale + 1) >= 0) {
            tmp_b[lb + extra_scale] = 0;
            extra_scale++;
            if (lb + extra_scale + 1 >= 78) break;
        }
    }

    int cur_scale = extra_scale;
    int sticky = 0;
    while (qlen < max_q) {
        uint8_t shifted_b[80];
        memcpy(shifted_b, db, (size_t)lb);
        if (cur_scale > 0)
            memset(shifted_b + lb, 0, (size_t)cur_scale);
        int sblen = lb + (cur_scale > 0 ? cur_scale : 0);

        uint8_t digit = 0;
        while (digits_cmp(rem, rlen, shifted_b, sblen) >= 0) {
            digits_sub_inplace(rem, rlen, shifted_b, sblen);
            digit++;
        }
        q[qlen++] = digit;

        /* Check if remainder is zero. */
        int rem_zero = 1;
        for (int i = 0; i < rlen; i++) if (rem[i] != 0) { rem_zero = 0; break; }
        if (rem_zero && cur_scale == 0) {
            break;
        }
        if (qlen == max_q) {
            break;
        }
        if (cur_scale > 0) {
            cur_scale--;
        } else {
            /* Multiply rem by 10. */
            if (rlen < 78) {
                rem[rlen++] = 0;
            } else {
                /* Compact leading zeros. */
                int lead = 0;
                while (lead < rlen && rem[lead] == 0) lead++;
                memmove(rem, rem + lead, (size_t)(rlen - lead));
                rlen -= lead;
                rem[rlen++] = 0;
            }
            qexp--;
        }
    }

    /* If we stopped early with cur_scale > 0, account for remaining scale in qexp. */
    qexp += cur_scale;

    /* Check remaining non-zero remainder for sticky bit. */
    for (int i = 0; i < rlen; i++) {
        if (rem[i] != 0) { sticky = 1; break; }
    }

    /* Trim trailing zeros only if exact (sticky == 0) and qexp < preferred a.exp - b.exp. */
    int64_t pref_exp = (int64_t)a.exp - (int64_t)b.exp;
    if (!sticky) {
        while (qlen > 1 && q[qlen - 1] == 0 && qexp < pref_exp) {
            qlen--;
            qexp++;
        }
    }

    if (qexp > 100000) return dec_inf(sign);
    if (qexp < -100000) {
        if (dec_should_round_up(sign, 0, 0, 1, rmode)) {
            uint8_t one = 1;
            return dec_pack_digits(sign, &one, 1, MT_DEC_ETINY);
        }
        return dec_zero(sign, MT_DEC_ETINY);
    }

    return dec_finalize(sign, q, qlen, (int32_t)qexp, sticky, rmode);
}

mt_dec128_t
mt_dec_rem(mt_dec128_t a, mt_dec128_t b, int rmode)
{
    if (a.kind != MT_DEC_FLAG_FINITE || b.kind == MT_DEC_FLAG_NAN || dec_is_zero(b))
        return dec_nan();
    if (b.kind == MT_DEC_FLAG_INF) return a;
    if (dec_is_zero(a)) return a;

    /* Compute integer quotient q = trunc(a / b), then a - q * b. */
    mt_dec128_t q = mt_dec_div(a, b, MT_DEC_ROUND_DOWN);
    q = mt_dec_quantize(q, 0, MT_DEC_ROUND_DOWN);
    if (q.kind != MT_DEC_FLAG_FINITE) return dec_nan();
    mt_dec128_t prod = mt_dec_mul(q, b, rmode);
    mt_dec128_t r = mt_dec_sub(a, prod, rmode);
    if (dec_is_zero(r)) r.sign = a.sign;
    return r;
}

mt_dec128_t
mt_dec_quantize(mt_dec128_t a, int32_t target_exp, int rmode)
{
    if (a.kind != MT_DEC_FLAG_FINITE) return dec_nan();
    if (target_exp < MT_DEC_ETINY || target_exp > MT_DEC_EMAX - MT_DEC_PREC + 1)
        return dec_nan();
    if (dec_is_zero(a))
        return dec_zero(a.sign, target_exp);

    uint8_t da[40];
    int la = dec_unpack(a, da);
    int32_t diff = target_exp - a.exp;

    if (diff == 0) return a;
    if (diff < 0) {
        /* Shift left by -diff zeros. */
        int32_t add_z = -diff;
        if (la + add_z > MT_DEC_PREC) return dec_nan();
        uint8_t buf[40];
        memcpy(buf, da, (size_t)la);
        memset(buf + la, 0, (size_t)add_z);
        return dec_pack_digits(a.sign, buf, la + (int)add_z, target_exp);
    }

    /* diff > 0: round off diff least-significant digits. */
    uint8_t kept[40];
    int klen;
    uint8_t first_dropped = 0;
    int sticky = 0;
    if (diff > la) {
        klen = 0;
        if (diff == la + 1) {
            first_dropped = da[0];
            for (int i = 1; i < la; i++) if (da[i]) { sticky = 1; break; }
        } else {
            sticky = 1;
        }
    } else {
        klen = la - (int)diff;
        memcpy(kept, da, (size_t)klen);
        first_dropped = da[klen];
        for (int i = klen + 1; i < la; i++) if (da[i]) { sticky = 1; break; }
    }

    uint8_t last_kept = (klen > 0) ? kept[klen - 1] : 0;
    if (dec_should_round_up(a.sign, last_kept, first_dropped, sticky, rmode)) {
        if (klen == 0) {
            kept[0] = 1;
            klen = 1;
        } else {
            int carry = 1;
            for (int i = klen - 1; i >= 0 && carry; i--) {
                int s = kept[i] + carry;
                if (s == 10) { kept[i] = 0; carry = 1; }
                else { kept[i] = (uint8_t)s; carry = 0; }
            }
            if (carry) {
                if (klen >= MT_DEC_PREC) return dec_nan();
                memmove(kept + 1, kept, (size_t)klen);
                kept[0] = 1;
                klen++;
            }
        }
    }
    return dec_pack_digits(a.sign, kept, klen, target_exp);
}

/* ---------------- Formatting & Serialization ---------------- */

size_t
mt_dec_to_string(mt_dec128_t a, char *buf, size_t bufsize)
{
    if (bufsize < 64) return 0;
    if (a.kind == MT_DEC_FLAG_NAN) {
        memcpy(buf, "NaN", 4);
        return 3;
    }
    if (a.kind == MT_DEC_FLAG_INF) {
        if (a.sign) { memcpy(buf, "-Infinity", 10); return 9; }
        memcpy(buf, "Infinity", 9); return 8;
    }

    uint8_t digits[40];
    int nd = dec_unpack(a, digits);
    if (nd == 0) {
        digits[0] = 0;
        nd = 1;
    }

    char *p = buf;
    if (a.sign) *p++ = '-';

    int32_t adj_exp = a.exp + nd - 1;
    /* IEEE 754-2008 to-scientific-string convention:
     * plain decimal if exp <= 0 and adj_exp >= -6. */
    if (a.exp <= 0 && adj_exp >= -6) {
        if (a.exp == 0) {
            for (int i = 0; i < nd; i++) *p++ = (char)('0' + digits[i]);
        } else if (adj_exp >= 0) {
            int ip = adj_exp + 1;
            for (int i = 0; i < ip; i++) *p++ = (char)('0' + digits[i]);
            *p++ = '.';
            for (int i = ip; i < nd; i++) *p++ = (char)('0' + digits[i]);
        } else {
            *p++ = '0';
            *p++ = '.';
            for (int i = 0; i < -adj_exp - 1; i++) *p++ = '0';
            for (int i = 0; i < nd; i++) *p++ = (char)('0' + digits[i]);
        }
    } else {
        *p++ = (char)('0' + digits[0]);
        if (nd > 1) {
            *p++ = '.';
            for (int i = 1; i < nd; i++) *p++ = (char)('0' + digits[i]);
        }
        *p++ = 'E';
        int32_t ae = adj_exp;
        if (ae < 0) { *p++ = '-'; ae = -ae; }
        else { *p++ = '+'; }
        char ebuf[16];
        int en = snprintf(ebuf, sizeof(ebuf), "%d", (int)ae);
        memcpy(p, ebuf, (size_t)en);
        p += en;
    }

    *p = '\0';
    return (size_t)(p - buf);
}

double
mt_dec_to_double(mt_dec128_t a)
{
    if (a.kind == MT_DEC_FLAG_NAN) {
        uint64_t q = UINT64_C(0x7ff8000000000000);
        double d; memcpy(&d, &q, 8); return d;
    }
    if (a.kind == MT_DEC_FLAG_INF) {
        uint64_t u = (a.sign ? UINT64_C(0x8000000000000000) : UINT64_C(0)) |
                     UINT64_C(0x7ff0000000000000);
        double d; memcpy(&d, &u, 8); return d;
    }
    char buf[64];
    mt_dec_to_string(a, buf, sizeof(buf));
    return strtod(buf, NULL);
}

/*
 * Canonical IEEE 754-2008 16-byte BID128 encoding (big-endian byte order
 * for cross-host digest stability).
 */
void
mt_dec_to_bid128(mt_dec128_t a, uint8_t out[16])
{
    uint64_t hi = 0, lo = 0;
    if (a.kind == MT_DEC_FLAG_NAN) {
        hi = UINT64_C(0x7c00000000000000); /* canonical positive qNaN */
        lo = 0;
    } else if (a.kind == MT_DEC_FLAG_INF) {
        hi = ((uint64_t)a.sign << 63) | UINT64_C(0x7800000000000000);
        lo = 0;
    } else {
        uint32_t biased_exp = (uint32_t)(a.exp + MT_DEC_BIAS);
        /* BID128 standard encoding:
         * bit 127: sign
         * bits 126..113: 14-bit biased exponent (when sig < 2^113)
         * bits 112..0: 113-bit significand (10^34 - 1 < 2^113) */
        hi = ((uint64_t)a.sign << 63) |
             (((uint64_t)(biased_exp & 0x3fffu)) << 49) |
             (a.hi & UINT64_C(0x0001ffffffffffff));
        lo = a.lo;
    }
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++)
        out[8 + i] = (uint8_t)(lo >> (56 - 8 * i));
}

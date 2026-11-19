/*
 * IEEE 754-2008 decimal128 arithmetic for MT::Decimal.
 *
 * Precision: 34 decimal digits (significand 0 .. 10^34 - 1).
 * Exponent range: Emin = -6143, Emax = 6144, Etiny = -6176.
 * Supports all 5 IEEE 754-2008 rounding modes, signed zero, +-Inf,
 * canonical qNaN, and canonical 16-byte BID128 serialization.
 *
 * Uses hardware _Decimal128 on POWER DFP targets when available and
 * a deterministic software BID128 engine everywhere else.
 */

#ifndef MT_DECIMAL_H
#define MT_DECIMAL_H

#include <stdint.h>
#include <stddef.h>

#define MT_DEC_PREC  34
#define MT_DEC_EMIN  (-6143)
#define MT_DEC_EMAX  6144
#define MT_DEC_ETINY (-6176) /* Emin - (prec - 1) */
#define MT_DEC_BIAS  6176

#define MT_DEC_FLAG_FINITE 0
#define MT_DEC_FLAG_INF    1
#define MT_DEC_FLAG_NAN    2

#define MT_DEC_ROUND_HALF_EVEN 0
#define MT_DEC_ROUND_HALF_UP   1
#define MT_DEC_ROUND_DOWN      2
#define MT_DEC_ROUND_FLOOR     3
#define MT_DEC_ROUND_CEIL      4

typedef struct {
    uint64_t lo;
    uint64_t hi;
    int32_t  exp;   /* value = (-1)^sign * (hi*2^64 + lo) * 10^exp */
    uint8_t  sign;  /* 0 = positive, 1 = negative */
    uint8_t  kind;  /* MT_DEC_FLAG_* */
    uint16_t pad;
} mt_dec128_t;

const char *mt_dec_backend(void);

mt_dec128_t mt_dec_from_string(const char *s, size_t len, int rmode);
mt_dec128_t mt_dec_from_i64(int64_t v);
mt_dec128_t mt_dec_from_double(double d, int rmode);

mt_dec128_t mt_dec_add(mt_dec128_t a, mt_dec128_t b, int rmode);
mt_dec128_t mt_dec_sub(mt_dec128_t a, mt_dec128_t b, int rmode);
mt_dec128_t mt_dec_mul(mt_dec128_t a, mt_dec128_t b, int rmode);
mt_dec128_t mt_dec_div(mt_dec128_t a, mt_dec128_t b, int rmode);
mt_dec128_t mt_dec_rem(mt_dec128_t a, mt_dec128_t b, int rmode);
mt_dec128_t mt_dec_neg(mt_dec128_t a);
mt_dec128_t mt_dec_abs(mt_dec128_t a);
mt_dec128_t mt_dec_quantize(mt_dec128_t a, int32_t target_exp, int rmode);

/* Returns -1, 0, +1 for ordered numbers, or 2 if either operand is NaN. */
int         mt_dec_cmp(mt_dec128_t a, mt_dec128_t b);
double      mt_dec_to_double(mt_dec128_t a);
size_t      mt_dec_to_string(mt_dec128_t a, char *buf, size_t bufsize);
void        mt_dec_to_bid128(mt_dec128_t a, uint8_t out[16]);

#endif /* MT_DECIMAL_H */

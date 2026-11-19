/*
 * perl5-mt: regex span kernels.
 *
 * The regex engine asks four questions of a run of bytes, and each has a
 * kernel here:
 *
 *   perl_simd_span_byte(s, len, c)            length of the leading run of c
 *   perl_simd_span_not_byte(s, len, c)        length of the leading run of non-c
 *   perl_simd_span_mask(s, len, byte, mask)   length of the leading run where
 *                                             (x & mask) == byte
 *   perl_simd_find_masked(s, len, byte, mask) first i where (x & mask) == byte,
 *                                             or len
 *   perl_simd_anyof_scan(s, len, bitmap, want_match)
 *                                             first i whose membership in the
 *                                             256-bit class bitmap is
 *                                             want_match, or len
 *   perl_simd_span_range(s, len, base, delta) and perl_simd_find_range(...)
 *                                             the same two questions for a
 *                                             single range base..base+delta
 *                                             (the ANYOFR nodes).  Exact only
 *                                             when base + delta <= 255; the
 *                                             hooks check that.
 *
 * Every kernel has a scalar twin, which is the upstream loop, and the
 * dispatch functions switch on PL_simd_level as in simd/perl_simd_bytes.h.
 *
 * The byte-compare kernels use GCC/Clang vector extensions: a 16-byte body on
 * the baseline unit of each host, and a 32-byte body under
 * target("avx2") on amd64.  The class kernel is a two-nibble lookup
 * (perl_simd_anyof_scan_shuf): for byte b, hi = b >> 4 selects a row of the
 * bitmap, bit 3 of b selects the low or high half of the row, and bit b & 7 of
 * that half is the answer.  The lookup is exact for all 256 byte values.  It
 * is built with __builtin_shuffle under GCC (pshufb, vperm or tbl by host) and
 * with vqtbl1q_u8 under Apple clang on arm64.  Clang on amd64 and every other
 * host take the scalar twin.
 *
 * No kernel reads outside [s, s + len): a vector body runs only while a whole
 * block fits, and the remainder is handled by the scalar loop.
 */

#ifndef PERL_SIMD_REGEX_H
#define PERL_SIMD_REGEX_H

typedef enum {
    PERL_SIMD_PRED_EQ,      /* x == c */
    PERL_SIMD_PRED_NE,      /* x != c */
    PERL_SIMD_PRED_MASK,    /* (x & mask) == c */
    PERL_SIMD_PRED_RANGE    /* (x - c) <= mask, in bytes: c..c+mask */
} perl_simd_pred_t;

static inline bool
perl_simd_pred_byte(U8 x, perl_simd_pred_t p, U8 c, U8 mask)
{
    switch (p) {
        case PERL_SIMD_PRED_EQ:    return x == c;
        case PERL_SIMD_PRED_NE:    return x != c;
        case PERL_SIMD_PRED_RANGE: return (U8) (x - c) <= mask;
        default:                   return (x & mask) == c;
    }
}

/* Scalar twins of the span and find kernels, starting at offset i.  The
 * vector bodies end with these for the bytes after their last whole block. */

static inline STRLEN
perl_simd_span_from_scalar(const U8 *s, STRLEN len, STRLEN i,
                           perl_simd_pred_t p, U8 c, U8 mask)
{
    while (i < len && perl_simd_pred_byte(s[i], p, c, mask)) {
        i++;
    }

    return i;
}

static inline STRLEN
perl_simd_find_from_scalar(const U8 *s, STRLEN len, STRLEN i,
                           perl_simd_pred_t p, U8 c, U8 mask)
{
    while (i < len && ! perl_simd_pred_byte(s[i], p, c, mask)) {
        i++;
    }

    return i;
}

/* Lane helpers.  A compare result holds 0xFF in matching lanes and 0 in the
 * others, so whole-word tests give the answer for a whole block. */

static inline bool
perl_simd_lanes_all_set(const U8 *lanes, size_t n)
{
    PERL_UINTMAX_T w;
    size_t i;

    for (i = 0; i < n; i += sizeof w) {
        memcpy(&w, lanes + i, sizeof w);
        if (w != ~ (PERL_UINTMAX_T) 0) {
            return FALSE;
        }
    }

    return TRUE;
}

static inline bool
perl_simd_lanes_any_set(const U8 *lanes, size_t n)
{
    PERL_UINTMAX_T w;
    size_t i;

    for (i = 0; i < n; i += sizeof w) {
        memcpy(&w, lanes + i, sizeof w);
        if (w) {
            return TRUE;
        }
    }

    return FALSE;
}

static inline size_t
perl_simd_first_clear_lane(const U8 *lanes, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++) {
        if (! lanes[k]) {
            return k;
        }
    }

    return n;
}

static inline size_t
perl_simd_first_set_lane(const U8 *lanes, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++) {
        if (lanes[k]) {
            return k;
        }
    }

    return n;
}

/* ---- scalar twins -------------------------------------------------------- */

static inline STRLEN
perl_simd_span_byte_scalar(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_from_scalar(s, len, 0, PERL_SIMD_PRED_EQ, c, 0);
}

static inline STRLEN
perl_simd_span_not_byte_scalar(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_from_scalar(s, len, 0, PERL_SIMD_PRED_NE, c, 0);
}

static inline STRLEN
perl_simd_span_mask_scalar(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_span_from_scalar(s, len, 0, PERL_SIMD_PRED_MASK, byte,
                                      mask);
}

static inline STRLEN
perl_simd_find_masked_scalar(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_find_from_scalar(s, len, 0, PERL_SIMD_PRED_MASK, byte,
                                      mask);
}

static inline STRLEN
perl_simd_span_range_scalar(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_span_from_scalar(s, len, 0, PERL_SIMD_PRED_RANGE, base,
                                      delta);
}

static inline STRLEN
perl_simd_find_range_scalar(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_find_from_scalar(s, len, 0, PERL_SIMD_PRED_RANGE, base,
                                      delta);
}

static inline STRLEN
perl_simd_anyof_scan_scalar_from(const U8 *s, STRLEN len, STRLEN i,
                                 const U8 *bitmap, bool want_match)
{
    for (; i < len; i++) {
        const U8 b = s[i];

        if ((int) ((bitmap[b >> 3] >> (b & 7)) & 1) == (int) want_match) {
            break;
        }
    }

    return i;
}

static inline STRLEN
perl_simd_anyof_scan_scalar(const U8 *s, STRLEN len, const U8 *bitmap,
                            bool want_match)
{
    return perl_simd_anyof_scan_scalar_from(s, len, 0, bitmap, want_match);
}

#if defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

/* The GCC shuffle body needs a one-register byte table lookup, which GCC has
 * on the three hosts; Clang has no variable shuffle, so Apple clang uses the
 * NEON intrinsics instead and clang on amd64 and POWER takes the scalar twin */
#  if defined(__GNUC__) && ! defined(__clang__) && (defined(__x86_64__) \
      || defined(__powerpc64__) || defined(__aarch64__))
#    define PERL_SIMD_ANYOF_SHUF_OK 1
#  endif

/* ---- 16-byte bodies: the predicate over one block ------------------------ */

static inline perl_simd_s16
perl_simd_pred16(const U8 *s, perl_simd_pred_t p, U8 c, U8 mask)
{
    perl_simd_v16 v;
    const perl_simd_v16 cv = (perl_simd_v16) { 0 } + c;
    const perl_simd_v16 mv = (perl_simd_v16) { 0 } + mask;

    memcpy(&v, s, 16);

    switch (p) {
        case PERL_SIMD_PRED_EQ:    return v == cv;
        case PERL_SIMD_PRED_NE:    return v != cv;
        case PERL_SIMD_PRED_RANGE: return (v - cv) <= mv;
        default:                   return (v & mv) == cv;
    }
}

/* The leading run of matching bytes, in 16-byte blocks */
static inline STRLEN
perl_simd_span16(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    STRLEN i = 0;

    while (i + 16 <= len) {
        perl_simd_s16 m = perl_simd_pred16(s + i, p, c, mask);
        U8 lanes[16];

        memcpy(lanes, &m, 16);
        if (! perl_simd_lanes_all_set(lanes, 16)) {
            return i + perl_simd_first_clear_lane(lanes, 16);
        }

        i += 16;
    }

    return perl_simd_span_from_scalar(s, len, i, p, c, mask);
}

/* The first matching byte, in 16-byte blocks */
static inline STRLEN
perl_simd_find16(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    STRLEN i = 0;

    while (i + 16 <= len) {
        perl_simd_s16 m = perl_simd_pred16(s + i, p, c, mask);
        U8 lanes[16];

        memcpy(lanes, &m, 16);
        if (perl_simd_lanes_any_set(lanes, 16)) {
            return i + perl_simd_first_set_lane(lanes, 16);
        }

        i += 16;
    }

    return perl_simd_find_from_scalar(s, len, i, p, c, mask);
}

#  ifdef PERL_SIMD_HAVE_W32

/* ---- 32-byte bodies, amd64 AVX2 ------------------------------------------ */

__attribute__((target("avx2")))
static inline perl_simd_s32
perl_simd_pred32(const U8 *s, perl_simd_pred_t p, U8 c, U8 mask)
{
    perl_simd_v32 v;
    const perl_simd_v32 cv = (perl_simd_v32) { 0 } + c;
    const perl_simd_v32 mv = (perl_simd_v32) { 0 } + mask;

    memcpy(&v, s, 32);

    switch (p) {
        case PERL_SIMD_PRED_EQ:    return v == cv;
        case PERL_SIMD_PRED_NE:    return v != cv;
        case PERL_SIMD_PRED_RANGE: return (v - cv) <= mv;
        default:                   return (v & mv) == cv;
    }
}

__attribute__((target("avx2")))
static inline STRLEN
perl_simd_span32(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    STRLEN i = 0;

    while (i + 32 <= len) {
        perl_simd_s32 m = perl_simd_pred32(s + i, p, c, mask);
        U8 lanes[32];

        memcpy(lanes, &m, 32);
        if (! perl_simd_lanes_all_set(lanes, 32)) {
            return i + perl_simd_first_clear_lane(lanes, 32);
        }

        i += 32;
    }

    return perl_simd_span_from_scalar(s, len, i, p, c, mask);
}

__attribute__((target("avx2")))
static inline STRLEN
perl_simd_find32(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    STRLEN i = 0;

    while (i + 32 <= len) {
        perl_simd_s32 m = perl_simd_pred32(s + i, p, c, mask);
        U8 lanes[32];

        memcpy(lanes, &m, 32);
        if (perl_simd_lanes_any_set(lanes, 32)) {
            return i + perl_simd_first_set_lane(lanes, 32);
        }

        i += 32;
    }

    return perl_simd_find_from_scalar(s, len, i, p, c, mask);
}

#  endif /* PERL_SIMD_HAVE_W32 */

/* ---- dispatch for the compare kernels ----------------------------------- */

static inline STRLEN
perl_simd_span_vec(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_span32(s, len, p, c, mask);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_span16(s, len, p, c, mask);
        default:
            return perl_simd_span_from_scalar(s, len, 0, p, c, mask);
    }
}

static inline STRLEN
perl_simd_find_vec(const U8 *s, STRLEN len, perl_simd_pred_t p, U8 c, U8 mask)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_find32(s, len, p, c, mask);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_find16(s, len, p, c, mask);
        default:
            return perl_simd_find_from_scalar(s, len, 0, p, c, mask);
    }
}

static inline STRLEN
perl_simd_span_byte(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_vec(s, len, PERL_SIMD_PRED_EQ, c, 0);
}

static inline STRLEN
perl_simd_span_not_byte(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_vec(s, len, PERL_SIMD_PRED_NE, c, 0);
}

static inline STRLEN
perl_simd_span_mask(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_span_vec(s, len, PERL_SIMD_PRED_MASK, byte, mask);
}

static inline STRLEN
perl_simd_find_masked(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_find_vec(s, len, PERL_SIMD_PRED_MASK, byte, mask);
}

static inline STRLEN
perl_simd_span_range(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_span_vec(s, len, PERL_SIMD_PRED_RANGE, base, delta);
}

static inline STRLEN
perl_simd_find_range(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_find_vec(s, len, PERL_SIMD_PRED_RANGE, base, delta);
}

/* ---- the class bitmap: two-nibble lookup -------------------------------- *
 *
 * Row r of the table is bitmap bytes 2r (members r*16 + 0..7) and 2r+1
 * (members r*16 + 8..15).  For each byte b, hi = b >> 4 picks the row, bit 3
 * of b picks the half, and bit b & 7 of that half is the membership.  The
 * bit for b & 15 is 1 << (b & 7), so one 16-entry table gives it. */

#  if defined(PERL_SIMD_ANYOF_SHUF_OK)

/* GCC: __builtin_shuffle is a one-register table lookup on every host:
 * pshufb under target("ssse3") on amd64, vperm on POWER, tbl on arm64 */

#    if defined(__x86_64__)
#      define PERL_SIMD_ANYOF_ATTR __attribute__((target("ssse3")))
#    else
#      define PERL_SIMD_ANYOF_ATTR
#    endif

PERL_SIMD_ANYOF_ATTR
static inline STRLEN
perl_simd_anyof_scan_shuf(const U8 *s, STRLEN len, const U8 *bitmap,
                          bool want_match)
{
    U8 row_lo[16], row_hi[16], bit[16];
    perl_simd_v16 ta, tb, tp;
    const perl_simd_v16 n0f = (perl_simd_v16) { 0 } + 0x0F;
    const perl_simd_v16 n8 = (perl_simd_v16) { 0 } + 0x08;
    const perl_simd_v16 zero = (perl_simd_v16) { 0 };
    const perl_simd_v16 flip = (perl_simd_v16) { 0 } + (U8) (want_match ? 0 : 0xFF);
    STRLEN i = 0;
    int k;

    for (k = 0; k < 16; k++) {
        row_lo[k] = bitmap[2 * k];
        row_hi[k] = bitmap[2 * k + 1];
        bit[k] = (U8) (1 << (k & 7));
    }
    memcpy(&ta, row_lo, 16);
    memcpy(&tb, row_hi, 16);
    memcpy(&tp, bit, 16);

    while (i + 16 <= len) {
        perl_simd_v16 v, row, sel, inset, hit;
        U8 lanes[16];

        memcpy(&v, s + i, 16);

        /* sel is 0xFF where bit 3 of the byte is set: the high half of row */
        sel = (perl_simd_v16) ((v & n8) != zero);
        row = (__builtin_shuffle(tb, v >> 4) & sel)
            | (__builtin_shuffle(ta, v >> 4) & ~sel);
        inset = (perl_simd_v16) ((row & __builtin_shuffle(tp, v & n0f))
                                 != zero);
        hit = inset ^ flip;

        memcpy(lanes, &hit, 16);
        if (perl_simd_lanes_any_set(lanes, 16)) {
            return i + perl_simd_first_set_lane(lanes, 16);
        }

        i += 16;
    }

    return perl_simd_anyof_scan_scalar_from(s, len, i, bitmap, want_match);
}

#    define PERL_SIMD_ANYOF_BODY perl_simd_anyof_scan_shuf

#  elif defined(__aarch64__) && defined(__ARM_NEON)

/* Apple clang on arm64: the same lookup with the NEON intrinsics */

#    include <arm_neon.h>

static inline STRLEN
perl_simd_anyof_scan_neon(const U8 *s, STRLEN len, const U8 *bitmap,
                          bool want_match)
{
    static const U8 bit_tab[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128
    };
    U8 row_lo[16], row_hi[16];
    uint8x16_t ta, tb, tp;
    const uint8x16_t n0f = vdupq_n_u8(0x0F);
    const uint8x16_t n8 = vdupq_n_u8(0x08);
    STRLEN i = 0;
    int k;

    for (k = 0; k < 16; k++) {
        row_lo[k] = bitmap[2 * k];
        row_hi[k] = bitmap[2 * k + 1];
    }
    ta = vld1q_u8(row_lo);
    tb = vld1q_u8(row_hi);
    tp = vld1q_u8(bit_tab);

    while (i + 16 <= len) {
        const uint8x16_t v = vld1q_u8(s + i);
        const uint8x16_t hi = vshrq_n_u8(v, 4);
        const uint8x16_t lo = vandq_u8(v, n0f);
        const uint8x16_t row = vbslq_u8(vtstq_u8(v, n8),
                                        vqtbl1q_u8(tb, hi),
                                        vqtbl1q_u8(ta, hi));
        const uint8x16_t inset = vtstq_u8(row, vqtbl1q_u8(tp, lo));
        const uint8x16_t hit = veorq_u8(inset, vdupq_n_u8(want_match ? 0 : 0xFF));
        U8 lanes[16];

        vst1q_u8(lanes, hit);
        if (perl_simd_lanes_any_set(lanes, 16)) {
            return i + perl_simd_first_set_lane(lanes, 16);
        }

        i += 16;
    }

    return perl_simd_anyof_scan_scalar_from(s, len, i, bitmap, want_match);
}

#    define PERL_SIMD_ANYOF_BODY perl_simd_anyof_scan_neon

#  endif /* the class bodies */

#  ifdef PERL_SIMD_ANYOF_BODY

/* The class lookup runs at the level its body needs: SSSE3 on amd64 (the
 * AVX2 level implies it), VSX on POWER, NEON on arm64 */
#    if defined(__x86_64__)
#      define PERL_SIMD_ANYOF_LEVEL PERL_SIMD_AVX2
#    elif defined(__powerpc64__)
#      define PERL_SIMD_ANYOF_LEVEL PERL_SIMD_VSX
#    else
#      define PERL_SIMD_ANYOF_LEVEL PERL_SIMD_NEON
#    endif

static inline STRLEN
perl_simd_anyof_scan(const U8 *s, STRLEN len, const U8 *bitmap,
                     bool want_match)
{
    switch (PL_simd_level) {
        case PERL_SIMD_ANYOF_LEVEL:
            return PERL_SIMD_ANYOF_BODY(s, len, bitmap, want_match);
        default:
            return perl_simd_anyof_scan_scalar(s, len, bitmap, want_match);
    }
}

#  else

static inline STRLEN
perl_simd_anyof_scan(const U8 *s, STRLEN len, const U8 *bitmap,
                     bool want_match)
{
    return perl_simd_anyof_scan_scalar(s, len, bitmap, want_match);
}

#  endif

#elif ! defined(PERL_FORCE_SCALAR)

/* No vector extensions: every kernel is its scalar twin */

static inline STRLEN
perl_simd_span_byte(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_byte_scalar(s, len, c);
}

static inline STRLEN
perl_simd_span_not_byte(const U8 *s, STRLEN len, U8 c)
{
    return perl_simd_span_not_byte_scalar(s, len, c);
}

static inline STRLEN
perl_simd_span_mask(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_span_mask_scalar(s, len, byte, mask);
}

static inline STRLEN
perl_simd_find_masked(const U8 *s, STRLEN len, U8 byte, U8 mask)
{
    return perl_simd_find_masked_scalar(s, len, byte, mask);
}

static inline STRLEN
perl_simd_span_range(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_span_range_scalar(s, len, base, delta);
}

static inline STRLEN
perl_simd_find_range(const U8 *s, STRLEN len, U8 base, U8 delta)
{
    return perl_simd_find_range_scalar(s, len, base, delta);
}

static inline STRLEN
perl_simd_anyof_scan(const U8 *s, STRLEN len, const U8 *bitmap,
                     bool want_match)
{
    return perl_simd_anyof_scan_scalar(s, len, bitmap, want_match);
}

#endif

#endif /* PERL_SIMD_REGEX_H */

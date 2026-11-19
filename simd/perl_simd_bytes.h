/*
 * perl5-mt: byte kernels.  Each kernel is a scalar twin, a 16-byte
 * vector body, a 32-byte AVX2 body (amd64 only) and a dispatch function that
 * switches on PL_simd_level.  See simd/perl_simd_dispatch.h for the gates.
 *
 * No kernel reads outside [s, s + len): a vector body runs only while a whole
 * block fits, and the remainder is handled by the scalar loop.
 */

#ifndef PERL_SIMD_BYTES_H
#define PERL_SIMD_BYTES_H

#ifdef PERL_SIMD_VEC
typedef U8 perl_simd_v16 __attribute__((vector_size(16)));
typedef I8 perl_simd_s16 __attribute__((vector_size(16)));
#  ifdef PERL_SIMD_HAVE_W32
typedef U8 perl_simd_v32 __attribute__((vector_size(32)));
typedef I8 perl_simd_s32 __attribute__((vector_size(32)));
#  endif
#endif

/* ---- perl_simd_ascii_span: length of the leading run of bytes whose high bit
 * is clear. ---------------------------------------------------------------- */

static inline STRLEN
perl_simd_ascii_span_scalar(const U8 *s, STRLEN len)
{
    STRLEN i = 0;

    while (i < len && ! (s[i] & 0x80)) {
        i++;
    }

    return i;
}

#if defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

static inline STRLEN
perl_simd_ascii_span_w16(const U8 *s, STRLEN len)
{
    STRLEN i = 0;

    /* Look at 64 bytes at a time; stop at the first block with a high bit and
     * let the scalar loop find the byte inside it */
    while (i + 64 <= len) {
        perl_simd_v16 a, b, c, d, acc;

        memcpy(&a, s + i,      16);
        memcpy(&b, s + i + 16, 16);
        memcpy(&c, s + i + 32, 16);
        memcpy(&d, s + i + 48, 16);

        acc = (a | b) | (c | d);
        if (perl_simd_bytes_have_high(&acc, sizeof acc)) {
            break;
        }

        i += 64;
    }

    while (i < len && ! (s[i] & 0x80)) {
        i++;
    }

    return i;
}

#  ifdef PERL_SIMD_HAVE_W32

__attribute__((target("avx2")))
static inline STRLEN
perl_simd_ascii_span_w32(const U8 *s, STRLEN len)
{
    STRLEN i = 0;

    while (i + 128 <= len) {
        perl_simd_v32 a, b, c, d, acc;

        memcpy(&a, s + i,      32);
        memcpy(&b, s + i + 32, 32);
        memcpy(&c, s + i + 64, 32);
        memcpy(&d, s + i + 96, 32);

        acc = (a | b) | (c | d);
        if (perl_simd_bytes_have_high(&acc, sizeof acc)) {
            break;
        }

        i += 128;
    }

    while (i < len && ! (s[i] & 0x80)) {
        i++;
    }

    return i;
}

#  endif /* PERL_SIMD_HAVE_W32 */

static inline STRLEN
perl_simd_ascii_span(const U8 *s, STRLEN len)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_ascii_span_w32(s, len);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_ascii_span_w16(s, len);
        default:
            return perl_simd_ascii_span_scalar(s, len);
    }
}

#elif ! defined(PERL_FORCE_SCALAR)

static inline STRLEN
perl_simd_ascii_span(const U8 *s, STRLEN len)
{
    return perl_simd_ascii_span_scalar(s, len);
}

#endif

/* ---- perl_simd_count_high: number of bytes whose high bit is set. ---------
 *
 * Vector bodies add the high bits into byte lanes, which hold at most 255
 * increments, and flush the lanes to a scalar count after each such run. */

static inline STRLEN
perl_simd_count_high_scalar(const U8 *s, STRLEN len)
{
    STRLEN i, count = 0;

    for (i = 0; i < len; i++) {
        count += (s[i] >> 7);
    }

    return count;
}

#if defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

static inline STRLEN
perl_simd_count_high_w16(const U8 *s, STRLEN len)
{
    STRLEN i = 0, count = 0;

    while (i + 16 <= len) {
        perl_simd_v16 acc;
        U8 lanes[16];
        unsigned int run = 0;
        unsigned int k;

        memset(&acc, 0, sizeof acc);
        while (run < 255 && i + 16 <= len) {
            perl_simd_v16 v;

            memcpy(&v, s + i, 16);
            acc += v >> 7;
            i += 16;
            run++;
        }

        memcpy(lanes, &acc, sizeof lanes);
        for (k = 0; k < 16; k++) {
            count += lanes[k];
        }
    }

    while (i < len) {
        count += (s[i] >> 7);
        i++;
    }

    return count;
}

#  ifdef PERL_SIMD_HAVE_W32

__attribute__((target("avx2")))
static inline STRLEN
perl_simd_count_high_w32(const U8 *s, STRLEN len)
{
    STRLEN i = 0, count = 0;

    while (i + 32 <= len) {
        perl_simd_v32 acc;
        U8 lanes[32];
        unsigned int run = 0;
        unsigned int k;

        memset(&acc, 0, sizeof acc);
        while (run < 255 && i + 32 <= len) {
            perl_simd_v32 v;

            memcpy(&v, s + i, 32);
            acc += v >> 7;
            i += 32;
            run++;
        }

        memcpy(lanes, &acc, sizeof lanes);
        for (k = 0; k < 32; k++) {
            count += lanes[k];
        }
    }

    while (i < len) {
        count += (s[i] >> 7);
        i++;
    }

    return count;
}

#  endif /* PERL_SIMD_HAVE_W32 */

static inline STRLEN
perl_simd_count_high(const U8 *s, STRLEN len)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_count_high_w32(s, len);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_count_high_w16(s, len);
        default:
            return perl_simd_count_high_scalar(s, len);
    }
}

#elif ! defined(PERL_FORCE_SCALAR)

static inline STRLEN
perl_simd_count_high(const U8 *s, STRLEN len)
{
    return perl_simd_count_high_scalar(s, len);
}

#endif

/* ---- 5c: the range-shift byte map and ASCII case -------------------------
 *
 * Each vector body returns exactly what its scalar twin returns; the rules are
 * stated per kernel.  Vector loads are made only for whole blocks inside the
 * caller's range, as above.
 *
 *   perl_simd_tr_range(s, len, lo, hi, d) in place: every byte in [lo, hi] is
 *                                        raised by d (mod 256); returns the count
 *   perl_simd_ascii_toupper/tolower(dst, src, len): the ASCII case map; dst and
 *                                        src are equal or do not overlap
 */

static inline STRLEN
perl_simd_tr_range_scalar(U8 *s, STRLEN len, U8 lo, U8 hi, U8 delta)
{
    STRLEN i, count = 0;
    const U8 span = hi - lo;

    for (i = 0; i < len; i++) {
        if ((U8) (s[i] - lo) <= span) {
            s[i] = (U8) (s[i] + delta);
            count++;
        }
    }

    return count;
}

static inline void
perl_simd_ascii_toupper_scalar(U8 *dst, const U8 *src, STRLEN len)
{
    STRLEN i;

    for (i = 0; i < len; i++) {
        dst[i] = (U8) ((src[i] >= 'a' && src[i] <= 'z') ? src[i] - 0x20 : src[i]);
    }
}

static inline void
perl_simd_ascii_tolower_scalar(U8 *dst, const U8 *src, STRLEN len)
{
    STRLEN i;

    for (i = 0; i < len; i++) {
        dst[i] = (U8) ((src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 0x20 : src[i]);
    }
}

#if defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

/* One body per width.  The masks are signed bytes, all ones or all zeros per
 * lane.  The counting in perl_simd_tr_range uses byte lanes that are flushed
 * every 255 blocks, as in perl_simd_count_high. */
#  define PERL_SIMD_BYTES_5C_BODY(SFX, W, VT, MT, ATTR)                       \
ATTR static inline VT                                                       \
perl_simd_bsplat_##SFX(U8 c)                                                \
{                                                                           \
    VT v;                                                                   \
    memset(&v, c, sizeof v);                                                \
    return v;                                                               \
}                                                                           \
                                                                            \
ATTR static inline STRLEN                                                   \
perl_simd_tr_range_##SFX(U8 *s, STRLEN len, U8 lo, U8 hi, U8 delta)        \
{                                                                           \
    const VT lov = perl_simd_bsplat_##SFX(lo);                              \
    const VT spanv = perl_simd_bsplat_##SFX((U8) (hi - lo));                \
    const VT dv = perl_simd_bsplat_##SFX(delta);                            \
    STRLEN i = 0, count = 0;                                                \
                                                                            \
    while (i + W <= len) {                                                  \
        VT acc;                                                             \
        U8 lanes[W];                                                        \
        unsigned int run = 0;                                               \
        unsigned int k;                                                     \
                                                                            \
        memset(&acc, 0, sizeof acc);                                        \
        while (run < 255 && i + W <= len) {                                 \
            VT v, sel;                                                      \
            MT inr;                                                         \
                                                                            \
            memcpy(&v, s + i, W);                                           \
            inr = ((v - lov) <= spanv);                                     \
            sel = (VT) inr & dv;                                            \
            v += sel;                                                       \
            memcpy(s + i, &v, W);                                           \
            acc -= (VT) inr;            /* a lane is 0xFF when it is in */  \
            i += W;                                                         \
            run++;                                                          \
        }                                                                   \
                                                                            \
        memcpy(lanes, &acc, sizeof lanes);                                  \
        for (k = 0; k < W; k++) {                                           \
            count += lanes[k];                                              \
        }                                                                   \
    }                                                                       \
                                                                            \
    for (; i < len; i++) {                                                  \
        if ((U8) (s[i] - lo) <= (U8) (hi - lo)) {                          \
            s[i] = (U8) (s[i] + delta);                                     \
            count++;                                                        \
        }                                                                   \
    }                                                                       \
    return count;                                                           \
}                                                                           \
                                                                            \
ATTR static inline void                                                     \
perl_simd_ascii_toupper_##SFX(U8 *dst, const U8 *src, STRLEN len)           \
{                                                                           \
    const VT av = perl_simd_bsplat_##SFX('a');                              \
    const VT zv = perl_simd_bsplat_##SFX(25);                               \
    const VT cv = perl_simd_bsplat_##SFX(0x20);                             \
    STRLEN i = 0;                                                           \
                                                                            \
    while (i + W <= len) {                                                  \
        VT v;                                                               \
        MT low;                                                             \
                                                                            \
        memcpy(&v, src + i, W);                                             \
        low = ((v - av) <= zv);                                             \
        v -= (VT) low & cv;                                                 \
        memcpy(dst + i, &v, W);                                             \
        i += W;                                                             \
    }                                                                       \
    for (; i < len; i++) {                                                  \
        dst[i] = (U8) ((src[i] >= 'a' && src[i] <= 'z') ? src[i] - 0x20     \
                                                        : src[i]);          \
    }                                                                       \
}                                                                           \
                                                                            \
ATTR static inline void                                                     \
perl_simd_ascii_tolower_##SFX(U8 *dst, const U8 *src, STRLEN len)           \
{                                                                           \
    const VT av = perl_simd_bsplat_##SFX('A');                              \
    const VT zv = perl_simd_bsplat_##SFX(25);                               \
    const VT cv = perl_simd_bsplat_##SFX(0x20);                             \
    STRLEN i = 0;                                                           \
                                                                            \
    while (i + W <= len) {                                                  \
        VT v;                                                               \
        MT up;                                                              \
                                                                            \
        memcpy(&v, src + i, W);                                             \
        up = ((v - av) <= zv);                                              \
        v += (VT) up & cv;                                                  \
        memcpy(dst + i, &v, W);                                             \
        i += W;                                                             \
    }                                                                       \
    for (; i < len; i++) {                                                  \
        dst[i] = (U8) ((src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 0x20    \
                                                        : src[i]);          \
    }                                                                       \
}

PERL_SIMD_BYTES_5C_BODY(w16, 16, perl_simd_v16, perl_simd_s16, )

#  ifdef PERL_SIMD_HAVE_W32
PERL_SIMD_BYTES_5C_BODY(w32, 32, perl_simd_v32, perl_simd_s32,
                        __attribute__((target("avx2"))))
#  endif

static inline STRLEN
perl_simd_tr_range(U8 *s, STRLEN len, U8 lo, U8 hi, U8 delta)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_tr_range_w32(s, len, lo, hi, delta);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_tr_range_w16(s, len, lo, hi, delta);
        default:
            return perl_simd_tr_range_scalar(s, len, lo, hi, delta);
    }
}

static inline void
perl_simd_ascii_toupper(U8 *dst, const U8 *src, STRLEN len)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            perl_simd_ascii_toupper_w32(dst, src, len);
            return;
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            perl_simd_ascii_toupper_w16(dst, src, len);
            return;
        default:
            perl_simd_ascii_toupper_scalar(dst, src, len);
            return;
    }
}

static inline void
perl_simd_ascii_tolower(U8 *dst, const U8 *src, STRLEN len)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            perl_simd_ascii_tolower_w32(dst, src, len);
            return;
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            perl_simd_ascii_tolower_w16(dst, src, len);
            return;
        default:
            perl_simd_ascii_tolower_scalar(dst, src, len);
            return;
    }
}

#endif /* PERL_SIMD_VEC && ! PERL_FORCE_SCALAR */

#if ! defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

/* No vector extensions: the dispatch names are the scalar twins */
static inline STRLEN
perl_simd_tr_range(U8 *s, STRLEN len, U8 lo, U8 hi, U8 delta)
{
    return perl_simd_tr_range_scalar(s, len, lo, hi, delta);
}

static inline void
perl_simd_ascii_toupper(U8 *dst, const U8 *src, STRLEN len)
{
    perl_simd_ascii_toupper_scalar(dst, src, len);
}

static inline void
perl_simd_ascii_tolower(U8 *dst, const U8 *src, STRLEN len)
{
    perl_simd_ascii_tolower_scalar(dst, src, len);
}

#endif

#endif /* PERL_SIMD_BYTES_H */

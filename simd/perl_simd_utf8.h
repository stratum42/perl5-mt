/*
 * perl5-mt: UTF-8 classification and the character count.
 *
 * perl_simd_utf8_classify(s, len, &consumed) takes a block that starts at a
 * character boundary and returns one of
 *
 *   PERL_SIMD_UTF8_ALL_ASCII    every byte is below 0x80; consumed = len;
 *   PERL_SIMD_UTF8_WELL_FORMED  the first `consumed` bytes are complete
 *                               well-formed RFC 3629 sequences: code points
 *                               U+0080 to U+10FFFF, no overlong, no surrogate,
 *                               nothing above U+10FFFF.  `consumed` is a
 *                               sequence boundary: a sequence that len cuts off
 *                               is left to the caller, and consumed > 0;
 *   PERL_SIMD_UTF8_UNSURE       anything else, malformed bytes included; the
 *                               caller runs Perl's own code on the block.
 *
 * Perl's UTF-8 is wider (surrogates, non-characters, Perl's extended forms),
 * so WELL_FORMED is a proof only for the narrow class above and every other
 * input is UNSURE.  Nothing is classified WELL_FORMED that Perl's code would
 * not count the same way.
 *
 * The test is per byte (perl_simd_utf8_bad_byte).  Byte j is bad when the
 * three bytes before it say it must be a continuation and it is not, or the
 * reverse; when it is a byte that never occurs (C0, C1, F5..FF); or when it is
 * the second byte of E0, ED, F0 or F4 outside the range that keeps the
 * sequence out of the overlong, surrogate and above-U+10FFFF classes.  The
 * bytes before the first byte of a call count as zero: the call starts at a
 * character boundary, so nothing before it requires anything.
 *
 * The scalar twin and both vector bodies evaluate exactly this test, so they
 * give the same answer on every input.  Vector bodies load a block only when
 * the whole block fits in [s, s + len).
 */

#ifndef PERL_SIMD_UTF8_H
#define PERL_SIMD_UTF8_H

#define PERL_SIMD_UTF8_UNSURE      0
#define PERL_SIMD_UTF8_ALL_ASCII   1
#define PERL_SIMD_UTF8_WELL_FORMED 2

/* Hooks classify windows of this many bytes, so that a malformed byte costs
 * at most one window of extra work before the caller falls back */
#define PERL_SIMD_UTF8_CHUNK 1024

#define PERL_SIMD_UTF8_IS_CONT(c) (((c) & 0xC0) == 0x80)

/* The number of bytes in the sequence that starts with c (1 for ASCII), or 0
 * when c never starts a well-formed sequence */
static inline STRLEN
perl_simd_utf8_seq_len(U8 c)
{
    if (c < 0x80)                  return 1;
    if (c >= 0xC2 && c <= 0xDF)    return 2;
    if (c >= 0xE0 && c <= 0xEF)    return 3;
    if (c >= 0xF0 && c <= 0xF4)    return 4;
    return 0;
}

/* The per-byte test; p1, p2, p3 are the three bytes before c */
static inline bool
perl_simd_utf8_bad_byte(U8 p3, U8 p2, U8 p1, U8 c)
{
    const bool req  = (p1 >= 0xC0) || (p2 >= 0xE0) || (p3 >= 0xF0);
    const bool cont = PERL_SIMD_UTF8_IS_CONT(c);

    if (req != cont) {
        return TRUE;
    }
    if (c == 0xC0 || c == 0xC1 || c >= 0xF5) {
        return TRUE;
    }
    if (p1 == 0xE0 && c < 0xA0)  return TRUE;   /* overlong 3-byte */
    if (p1 == 0xED && c > 0x9F)  return TRUE;   /* surrogates */
    if (p1 == 0xF0 && c < 0x90)  return TRUE;   /* overlong 4-byte */
    if (p1 == 0xF4 && c > 0x8F)  return TRUE;   /* above U+10FFFF */
    return FALSE;
}

/* Called once the per-byte test has passed on [0, len).  Every sequence that
 * starts before the last character start is complete, or is cut off by len;
 * the continuation bytes after that start are all required. */
static inline int
perl_simd_utf8_finish(const U8 *s, STRLEN len, STRLEN *consumed)
{
    STRLEN p = len - 1;
    STRLEN end;

    while (p > 0 && PERL_SIMD_UTF8_IS_CONT(s[p])) {
        p--;
    }

    /* s[p] is the last character start.  Its sequence is cut off by len when
     * it needs more bytes than are left, and then the boundary is p. */
    end = (p + perl_simd_utf8_seq_len(s[p]) > len) ? p : len;
    if (end == 0) {
        *consumed = 0;
        return PERL_SIMD_UTF8_UNSURE;
    }

    *consumed = end;
    return PERL_SIMD_UTF8_WELL_FORMED;
}

static inline STRLEN
perl_simd_utf8_char_count_scalar(const U8 *s, STRLEN len)
{
    STRLEN i, count = 0;

    for (i = 0; i < len; i++) {
        count += ! PERL_SIMD_UTF8_IS_CONT(s[i]);
    }

    return count;
}

static inline int
perl_simd_utf8_classify_scalar(const U8 *s, STRLEN len, STRLEN *consumed)
{
    STRLEN i;

    *consumed = 0;
    if (perl_simd_ascii_span_scalar(s, len) == len) {
        *consumed = len;
        return PERL_SIMD_UTF8_ALL_ASCII;
    }

    for (i = 0; i < len; i++) {
        const U8 p1 = (i >= 1) ? s[i - 1] : 0;
        const U8 p2 = (i >= 2) ? s[i - 2] : 0;
        const U8 p3 = (i >= 3) ? s[i - 3] : 0;

        if (perl_simd_utf8_bad_byte(p3, p2, p1, s[i])) {
            return PERL_SIMD_UTF8_UNSURE;
        }
    }

    return perl_simd_utf8_finish(s, len, consumed);
}

#if defined(PERL_SIMD_VEC) && ! defined(PERL_FORCE_SCALAR)

/* The vector bodies, one per width.  The masks are signed bytes: all ones or
 * all zeros per lane, so the per-byte test is a few compares and bit
 * operations.  The lookback bytes come from a copy of the previous three
 * bytes and the block, so no load starts before s. */

#  define PERL_SIMD_UTF8_VEC_BODY(SFX, W, VT, MT, ATTR)                       \
ATTR static inline VT                                                       \
perl_simd_utf8_splat_##SFX(U8 c)                                            \
{                                                                           \
    VT v;                                                                   \
    memset(&v, c, sizeof v);                                                \
    return v;                                                               \
}                                                                           \
                                                                            \
ATTR static inline MT                                                       \
perl_simd_utf8_bad_##SFX(VT p3, VT p2, VT p1, VT c)                         \
{                                                                           \
    const VT k80 = perl_simd_utf8_splat_##SFX(0x80);                        \
    const VT kC0 = perl_simd_utf8_splat_##SFX(0xC0);                        \
    const VT kE0 = perl_simd_utf8_splat_##SFX(0xE0);                        \
    const VT kF0 = perl_simd_utf8_splat_##SFX(0xF0);                        \
    const MT req  = (p1 >= kC0) | (p2 >= kE0) | (p3 >= kF0);               \
    const MT cont = ((c & perl_simd_utf8_splat_##SFX(0xC0)) == k80);       \
    MT bad = req ^ cont;                                                    \
    bad |= (c >= perl_simd_utf8_splat_##SFX(0xF5));                         \
    bad |= (c == kC0);                                                      \
    bad |= (c == perl_simd_utf8_splat_##SFX(0xC1));                         \
    bad |= (p1 == kE0) & (c < perl_simd_utf8_splat_##SFX(0xA0));           \
    bad |= (p1 == perl_simd_utf8_splat_##SFX(0xED))                         \
           & (c > perl_simd_utf8_splat_##SFX(0x9F));                        \
    bad |= (p1 == kF0) & (c < perl_simd_utf8_splat_##SFX(0x90));           \
    bad |= (p1 == perl_simd_utf8_splat_##SFX(0xF4))                         \
           & (c > perl_simd_utf8_splat_##SFX(0x8F));                        \
    return bad;                                                             \
}                                                                           \
                                                                            \
ATTR static inline int                                                      \
perl_simd_utf8_classify_##SFX(const U8 *s, STRLEN len, STRLEN *consumed)    \
{                                                                           \
    STRLEN i = 0;                                                           \
    MT err;                                                                 \
                                                                            \
    *consumed = 0;                                                          \
    if (perl_simd_ascii_span_##SFX(s, len) == len) {                        \
        *consumed = len;                                                    \
        return PERL_SIMD_UTF8_ALL_ASCII;                                    \
    }                                                                       \
                                                                            \
    memset(&err, 0, sizeof err);                                            \
    if (len >= W) {                                                         \
        /* the first block: the three bytes before s count as zero */       \
        U8 buf[W + 3];                                                      \
        VT p3, p2, p1, cur;                                                 \
                                                                            \
        memset(buf, 0, 3);                                                  \
        memcpy(buf + 3, s, W);                                              \
        memcpy(&p3, buf,     W);                                            \
        memcpy(&p2, buf + 1, W);                                            \
        memcpy(&p1, buf + 2, W);                                            \
        memcpy(&cur, buf + 3, W);                                           \
        err |= perl_simd_utf8_bad_##SFX(p3, p2, p1, cur);                   \
        i = W;                                                              \
    }                                                                       \
    /* later blocks load their lookback bytes straight from s: i >= W > 3 */ \
    while (i + W <= len) {                                                  \
        VT p3, p2, p1, cur;                                                 \
                                                                            \
        memcpy(&p3, s + i - 3, W);                                          \
        memcpy(&p2, s + i - 2, W);                                          \
        memcpy(&p1, s + i - 1, W);                                          \
        memcpy(&cur, s + i, W);                                             \
        err |= perl_simd_utf8_bad_##SFX(p3, p2, p1, cur);                   \
        i += W;                                                             \
    }                                                                       \
    if (perl_simd_bytes_have_high(&err, sizeof err)) {                      \
        return PERL_SIMD_UTF8_UNSURE;                                       \
    }                                                                       \
                                                                            \
    for (; i < len; i++) {                                                  \
        const U8 p1 = (i >= 1) ? s[i - 1] : 0;                              \
        const U8 p2 = (i >= 2) ? s[i - 2] : 0;                              \
        const U8 p3 = (i >= 3) ? s[i - 3] : 0;                              \
                                                                            \
        if (perl_simd_utf8_bad_byte(p3, p2, p1, s[i])) {                    \
            return PERL_SIMD_UTF8_UNSURE;                                   \
        }                                                                   \
    }                                                                       \
                                                                            \
    return perl_simd_utf8_finish(s, len, consumed);                         \
}                                                                           \
                                                                            \
ATTR static inline STRLEN                                                   \
perl_simd_utf8_char_count_##SFX(const U8 *s, STRLEN len)                    \
{                                                                           \
    STRLEN i = 0, conts = 0;                                                \
                                                                            \
    while (i + W <= len) {                                                  \
        VT acc;                                                             \
        U8 lanes[W];                                                        \
        unsigned int run = 0;                                               \
        unsigned int k;                                                     \
                                                                            \
        memset(&acc, 0, sizeof acc);                                        \
        while (run < 255 && i + W <= len) {                                 \
            VT v, one_if_cont;                                              \
                                                                            \
            memcpy(&v, s + i, W);                                           \
            one_if_cont = (VT) ((v & perl_simd_utf8_splat_##SFX(0xC0))      \
                                == perl_simd_utf8_splat_##SFX(0x80));       \
            acc -= one_if_cont;      /* a lane is 0xFF when it is 1 */      \
            i += W;                                                         \
            run++;                                                          \
        }                                                                   \
                                                                            \
        memcpy(lanes, &acc, sizeof lanes);                                  \
        for (k = 0; k < W; k++) {                                           \
            conts += lanes[k];                                              \
        }                                                                   \
    }                                                                       \
                                                                            \
    for (; i < len; i++) {                                                  \
        conts += PERL_SIMD_UTF8_IS_CONT(s[i]);                              \
    }                                                                       \
                                                                            \
    return len - conts;                                                     \
}

PERL_SIMD_UTF8_VEC_BODY(w16, 16, perl_simd_v16, perl_simd_s16, )

#  ifdef PERL_SIMD_HAVE_W32

PERL_SIMD_UTF8_VEC_BODY(w32, 32, perl_simd_v32, perl_simd_s32,
                        __attribute__((target("avx2"))))

#  endif /* PERL_SIMD_HAVE_W32 */

static inline int
perl_simd_utf8_classify(const U8 *s, STRLEN len, STRLEN *consumed)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_utf8_classify_w32(s, len, consumed);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_utf8_classify_w16(s, len, consumed);
        default:
            return perl_simd_utf8_classify_scalar(s, len, consumed);
    }
}

static inline STRLEN
perl_simd_utf8_char_count(const U8 *s, STRLEN len)
{
    switch (PL_simd_level) {
#  ifdef PERL_SIMD_HAVE_W32
        case PERL_SIMD_AVX2:
            return perl_simd_utf8_char_count_w32(s, len);
#  endif
        case PERL_SIMD_SSE2:
        case PERL_SIMD_VSX:
        case PERL_SIMD_NEON:
            return perl_simd_utf8_char_count_w16(s, len);
        default:
            return perl_simd_utf8_char_count_scalar(s, len);
    }
}

#elif ! defined(PERL_FORCE_SCALAR)

static inline int
perl_simd_utf8_classify(const U8 *s, STRLEN len, STRLEN *consumed)
{
    return perl_simd_utf8_classify_scalar(s, len, consumed);
}

static inline STRLEN
perl_simd_utf8_char_count(const U8 *s, STRLEN len)
{
    return perl_simd_utf8_char_count_scalar(s, len);
}

#endif

#endif /* PERL_SIMD_UTF8_H */

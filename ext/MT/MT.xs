/*    MT.xs
 *
 *    perl5-mt: the ext/MT distribution and MT::Array packed
 *    array constructors, reductions, and vector operations.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#define PERL_NO_GET_CONTEXT
#define PERL_XS_THREADSAFE
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include <math.h>
#include <float.h>

#include "accel/perl_accel.h"

#if !defined(PERL_FORCE_SCALAR)
#  if defined(__aarch64__)
#    include <arm_neon.h>
#  elif defined(__SSE2__)
#    include <emmintrin.h>
#  endif
#endif

/* ---- Correctly-rounded Shewchuk EFT accumulator ------------------------- */

#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 6)
#  pragma STDC FP_CONTRACT OFF
#endif

struct mt_shewchuk {
    double *e;
    size_t  len;
    size_t  cap;
    double  inline_buf[64];
};

static inline void
shewchuk_init(struct mt_shewchuk *acc)
{
    acc->e   = acc->inline_buf;
    acc->len = 0;
    acc->cap = 64;
}

static inline void
shewchuk_free(struct mt_shewchuk *acc)
{
    if (acc->e != acc->inline_buf)
        Safefree(acc->e);
}

static void
shewchuk_reserve(struct mt_shewchuk *acc, size_t need)
{
    if (need <= acc->cap)
        return;
    {
        size_t newcap = acc->cap * 2;
        double *newe;
        while (newcap < need)
            newcap *= 2;
        Newx(newe, newcap, double);
        Copy(acc->e, newe, acc->len, double);
        if (acc->e != acc->inline_buf)
            Safefree(acc->e);
        acc->e   = newe;
        acc->cap = newcap;
    }
}

static inline void
mt_two_sum(double a, double b, double *s, double *err)
{
    double x  = a + b;
    double bv = x - a;
    double av = x - bv;
    double br = b - bv;
    double ar = a - av;
    *s   = x;
    *err = ar + br;
}

static inline void
mt_two_prod(double a, double b, double *p, double *err)
{
    double prod = a * b;
    *p   = prod;
    *err = fma(a, b, -prod);
}

static void
shewchuk_grow(struct mt_shewchuk *acc, double b)
{
    size_t i, out = 0;
    double q = b;

    if (q == 0.0)
        return;

    for (i = 0; i < acc->len; i++) {
        double s, err;
        mt_two_sum(q, acc->e[i], &s, &err);
        q = s;
        if (err != 0.0)
            acc->e[out++] = err;
    }
    if (q != 0.0) {
        if (out >= acc->cap)
            shewchuk_reserve(acc, out + 1);
        acc->e[out++] = q;
    }
    acc->len = out;
}

static void
shewchuk_compress(struct mt_shewchuk *acc)
{
    size_t n = acc->len;
    size_t bottom, top, i;
    double q;

    if (n <= 1)
        return;

    bottom = n - 1;
    q = acc->e[bottom];
    for (i = n - 1; i-- > 0; ) {
        double s, err;
        mt_two_sum(q, acc->e[i], &s, &err);
        if (err != 0.0) {
            acc->e[bottom--] = s;
            q = err;
        } else {
            q = s;
        }
    }
    acc->e[bottom] = q;

    top = 0;
    for (i = bottom + 1; i < n; i++) {
        double s, err;
        mt_two_sum(acc->e[i], q, &s, &err);
        if (err != 0.0)
            acc->e[top++] = err;
        q = s;
    }
    if (q != 0.0)
        acc->e[top++] = q;
    acc->len = top;
}

static double
shewchuk_round(struct mt_shewchuk *acc, bool all_neg_zero)
{
    shewchuk_compress(acc);
    shewchuk_compress(acc);

    if (acc->len == 0)
        return all_neg_zero ? -0.0 : 0.0;
    if (acc->len == 1)
        return acc->e[0];

    {
        double hi = acc->e[acc->len - 1];
        double lo = acc->e[acc->len - 2];
        double s, r;
        mt_two_sum(hi, lo, &s, &r);
        if (acc->len >= 3 && r != 0.0 && fabs(r) == fabs(lo)) {
            bool is_half = (s != hi)
                ? (fabs(s - hi) == fabs(2.0 * lo))
                : (fabs((hi + 2.0 * lo) - hi) == fabs(2.0 * lo));
            if (is_half) {
                double rem = acc->e[acc->len - 3];
                if ((lo > 0.0) == (rem > 0.0))
                    return nextafter(hi, copysign(INFINITY, lo));
                return hi;
            }
        }
        return s;
    }
}

/* ---- Helpers for accel / i8 conversion ---------------------------------- */

static const char *
mt_accel_device_class_name(perl_accel_device_class_t cls)
{
    switch (cls) {
        case PERL_ACCEL_DEV_CPU:    return "cpu";
        case PERL_ACCEL_DEV_GPU:    return "gpu";
        case PERL_ACCEL_DEV_NPU:    return "npu";
        case PERL_ACCEL_DEV_COPROC: return "coproc";
        default:                    return "unknown";
    }
}

static void
mt_av_to_i8(pTHX_ AV *av, SSize_t n, int8_t *dst, const char *fn)
{
    SSize_t i;
    if (SvPACKED_AV(av)) {
        U8 ptype = AvPACKED_TYPE(av);
        if (ptype == AV_PACKED_U8) {
            Copy((const char *)AvPACKED_DATA(av), (char *)dst, (Size_t)n, char);
            return;
        }
        if (ptype == AV_PACKED_IV) {
            const IV *p = (const IV *)AvPACKED_DATA(av);
            for (i = 0; i < n; i++) {
                if (p[i] < -128 || p[i] > 127)
                    Perl_croak(aTHX_ "MT::Array::%s: element %" IVdf " out of int8 range (-128..127)",
                               fn, p[i]);
                dst[i] = (int8_t)p[i];
            }
            return;
        }
    }
    for (i = 0; i < n; i++) {
        SV **svp = av_fetch(av, i, 0);
        IV v = (svp && *svp && SvOK(*svp)) ? SvIV(*svp) : 0;
        if (v < -128 || v > 127)
            Perl_croak(aTHX_ "MT::Array::%s: element %" IVdf " out of int8 range (-128..127)",
                       fn, v);
        dst[i] = (int8_t)v;
    }
}

/* ---- Helpers for IV / U8 / NV reductions -------------------------------- */

static SV *
mt_i128_to_sv(pTHX_ __int128 acc)
{
    if (acc >= (__int128)IV_MIN && acc <= (__int128)IV_MAX)
        return newSViv((IV)acc);
    if (acc > 0 && (unsigned __int128)acc <= (unsigned __int128)UV_MAX)
        return newSVuv((UV)acc);
    return newSVnv((NV)acc);
}

static __int128
mt_iv_sum(pTHX_ const IV *p, SSize_t n)
{
    __int128 acc = 0;
    SSize_t i = 0;
    PERL_UNUSED_CONTEXT;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 4) {
        /* Accumulate in chunks of 256 pairs into int64x2? No: IV can be near IV_MAX!
         * Use 128-bit lane-wise widening or add directly in __int128. */
        __int128 a0 = 0, a1 = 0;
        for (; i + 1 < n; i += 2) {
            int64x2_t v = vld1q_s64((const int64_t *)(p + i));
            a0 += (__int128)vgetq_lane_s64(v, 0);
            a1 += (__int128)vgetq_lane_s64(v, 1);
        }
        acc = a0 + a1;
    }
#endif
    for (; i < n; i++)
        acc += (__int128)p[i];
    return acc;
}

static IV
mt_iv_min(pTHX_ const IV *p, SSize_t n)
{
    IV m = p[0];
    SSize_t i = 1;
    PERL_UNUSED_CONTEXT;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 4) {
        int64x2_t vm = vld1q_s64((const int64_t *)p);
        i = 2;
        for (; i + 1 < n; i += 2) {
            int64x2_t v = vld1q_s64((const int64_t *)(p + i));
            uint64x2_t cmp = vcltq_s64(v, vm);
            vm = vbslq_s64(cmp, v, vm);
        }
        {
            IV m0 = (IV)vgetq_lane_s64(vm, 0);
            IV m1 = (IV)vgetq_lane_s64(vm, 1);
            m = m0 < m1 ? m0 : m1;
        }
    }
#endif
    for (; i < n; i++)
        if (p[i] < m) m = p[i];
    return m;
}

static IV
mt_iv_max(pTHX_ const IV *p, SSize_t n)
{
    IV m = p[0];
    SSize_t i = 1;
    PERL_UNUSED_CONTEXT;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 4) {
        int64x2_t vm = vld1q_s64((const int64_t *)p);
        i = 2;
        for (; i + 1 < n; i += 2) {
            int64x2_t v = vld1q_s64((const int64_t *)(p + i));
            uint64x2_t cmp = vcgtq_s64(v, vm);
            vm = vbslq_s64(cmp, v, vm);
        }
        {
            IV m0 = (IV)vgetq_lane_s64(vm, 0);
            IV m1 = (IV)vgetq_lane_s64(vm, 1);
            m = m0 > m1 ? m0 : m1;
        }
    }
#endif
    for (; i < n; i++)
        if (p[i] > m) m = p[i];
    return m;
}

static double
mt_nv_min(pTHX_ const double *p, SSize_t n)
{
    double m = p[0];
    SSize_t i = 1;
    PERL_UNUSED_CONTEXT;
    if (UNLIKELY(isnan(m)))
        return m;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 4) {
        float64x2_t vm = vld1q_f64(p);
        i = 2;
        for (; i + 1 < n; i += 2) {
            float64x2_t v = vld1q_f64(p + i);
            vm = vminq_f64(vm, v); /* IEEE-754 NaN-propagating */
        }
        {
            double m0 = vgetq_lane_f64(vm, 0);
            double m1 = vgetq_lane_f64(vm, 1);
            if (isnan(m0)) return m0;
            if (isnan(m1)) return m1;
            m = m0 < m1 ? m0 : m1;
        }
    }
#endif
    for (; i < n; i++) {
        double v = p[i];
        if (UNLIKELY(isnan(v))) return v;
        if (v < m) m = v;
    }
    return m;
}

static double
mt_nv_max(pTHX_ const double *p, SSize_t n)
{
    double m = p[0];
    SSize_t i = 1;
    PERL_UNUSED_CONTEXT;
    if (UNLIKELY(isnan(m)))
        return m;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 4) {
        float64x2_t vm = vld1q_f64(p);
        i = 2;
        for (; i + 1 < n; i += 2) {
            float64x2_t v = vld1q_f64(p + i);
            vm = vmaxq_f64(vm, v); /* IEEE-754 NaN-propagating */
        }
        {
            double m0 = vgetq_lane_f64(vm, 0);
            double m1 = vgetq_lane_f64(vm, 1);
            if (isnan(m0)) return m0;
            if (isnan(m1)) return m1;
            m = m0 > m1 ? m0 : m1;
        }
    }
#endif
    for (; i < n; i++) {
        double v = p[i];
        if (UNLIKELY(isnan(v))) return v;
        if (v > m) m = v;
    }
    return m;
}

static UV
mt_u8_count_scalar(const U8 *p, SSize_t n, U8 byte)
{
    UV cnt = 0;
    SSize_t i;
    for (i = 0; i < n; i++)
        if (p[i] == byte) cnt++;
    return cnt;
}

static UV
mt_u8_count(pTHX_ const U8 *p, SSize_t n, U8 byte)
{
    PERL_UNUSED_CONTEXT;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
    if (PERL_SIMD_ON() && n >= 16) {
        uint8x16_t target = vdupq_n_u8(byte);
        UV total = 0;
        SSize_t i = 0;
        while (i + 16 <= n) {
            uint8x16_t acc = vdupq_n_u8(0);
            SSize_t chunk = (n - i) / 16;
            SSize_t k;
            if (chunk > 255) chunk = 255;
            for (k = 0; k < chunk; k++, i += 16) {
                uint8x16_t v = vld1q_u8(p + i);
                uint8x16_t eq = vceqq_u8(v, target);
                acc = vsubq_u8(acc, eq); /* eq is 0xFF (-1) on match */
            }
            total += (UV)vaddlvq_u8(acc);
        }
        for (; i < n; i++)
            if (p[i] == byte) total++;
        return total;
    }
#elif !defined(PERL_FORCE_SCALAR) && defined(__SSE2__)
    if (PERL_SIMD_ON() && n >= 16) {
        __m128i target = _mm_set1_epi8((char)byte);
        UV total = 0;
        SSize_t i = 0;
        for (; i + 16 <= n; i += 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
            int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(v, target));
            total += (UV)__builtin_popcount((unsigned int)mask);
        }
        for (; i < n; i++)
            if (p[i] == byte) total++;
        return total;
    }
#endif
    return mt_u8_count_scalar(p, n, byte);
}

static AV *
mt_expect_av(pTHX_ SV *ref, const char *fn)
{
    if (!SvROK(ref) || SvTYPE(SvRV(ref)) != SVt_PVAV)
        Perl_croak(aTHX_ "MT::Array::%s expects an array reference", fn);
    return (AV *)SvRV(ref);
}

static U8
mt_parse_ptype(pTHX_ SV *typesv)
{
    STRLEN len;
    const char *s = SvPV_const(typesv, len);
    if (len == 2 && s[0] == 'I' && s[1] == 'V') return AV_PACKED_IV;
    if (len == 2 && s[0] == 'N' && s[1] == 'V') return AV_PACKED_NV;
    if (len == 2 && s[0] == 'U' && s[1] == '8') return AV_PACKED_U8;
    Perl_croak(aTHX_ "MT::Array: unknown packed type '%.*s' (expected IV, NV, or U8)",
               (int)len, s);
    return AV_PACKED_NONE;
}

MODULE = MT		PACKAGE = MT

PROTOTYPES: DISABLE

bool
_core_present()
    CODE:
        RETVAL = get_cv("MT::Channel::_new", 0) != NULL
              && get_cv("MT::Fiber::spawn", 0) != NULL
              && get_cv("MT::IO::stat", 0) != NULL;
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Array

PROTOTYPES: DISABLE

SV *
packed(typesv, ...)
        SV *typesv
    PREINIT:
        U8 ptype;
        SSize_t count, i;
        AV *av;
    CODE:
        ptype = mt_parse_ptype(aTHX_ typesv);
        count = items - 1;
        av = Perl_av_new_packed(aTHX_ ptype, count);
        for (i = 0; i < count; i++) {
            SV *sv = ST(i + 1);
            SvGETMAGIC(sv);
            if (!av_store(av, i, SvREFCNT_inc(sv))) {
                SvREFCNT_dec(sv);
            }
            if (!(SvFLAGS(av) & SVf_PACKED_AV)) {
                SvREFCNT_dec(av);
                Perl_croak(aTHX_ "MT::Array::packed: element %" IVdf " does not fit type", (IV)i);
            }
        }
        RETVAL = newRV_noinc((SV *)av);
    OUTPUT:
        RETVAL

SV *
pack(aref, ...)
        SV *aref
    PREINIT:
        AV *av;
    CODE:
        av = mt_expect_av(aTHX_ aref, "pack");
        if (items >= 2 && SvOK(ST(1))) {
            U8 ptype = mt_parse_ptype(aTHX_ ST(1));
            if (!Perl_av_pack(aTHX_ av, ptype))
                Perl_croak(aTHX_ "MT::Array::pack: array elements do not fit requested type");
        } else {
            if (!Perl_av_pack(aTHX_ av, AV_PACKED_IV)
                && !Perl_av_pack(aTHX_ av, AV_PACKED_NV))
            {
                Perl_croak(aTHX_ "MT::Array::pack: array elements do not fit IV or NV");
            }
        }
        RETVAL = SvREFCNT_inc(aref);
    OUTPUT:
        RETVAL

SV *
unpack(aref)
        SV *aref
    PREINIT:
        AV *av;
    CODE:
        av = mt_expect_av(aTHX_ aref, "unpack");
        Perl_av_unpack(aTHX_ av);
        RETVAL = SvREFCNT_inc(aref);
    OUTPUT:
        RETVAL

SV *
is_packed(aref)
        SV *aref
    PREINIT:
        AV *av;
        U8 ptype;
    CODE:
        av = mt_expect_av(aTHX_ aref, "is_packed");
        ptype = AvPACKED_TYPE(av);
        if (ptype == AV_PACKED_IV)      RETVAL = newSVpvs("IV");
        else if (ptype == AV_PACKED_NV) RETVAL = newSVpvs("NV");
        else if (ptype == AV_PACKED_U8) RETVAL = newSVpvs("U8");
        else                            RETVAL = newSViv(0);
    OUTPUT:
        RETVAL

UV
unpack_count()
    CODE:
        RETVAL = Perl_av_unpack_count();
    OUTPUT:
        RETVAL

SV *
sum(aref)
        SV *aref
    PREINIT:
        AV *av;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "sum");
        n  = AvFILL(av) + 1;
        if (n <= 0) {
            RETVAL = newSViv(0);
        } else if (SvPACKED_AV(av)) {
            U8 ptype = AvPACKED_TYPE(av);
            if (ptype == AV_PACKED_IV) {
                __int128 acc = mt_iv_sum(aTHX_ (const IV *)AvPACKED_DATA(av), n);
                RETVAL = mt_i128_to_sv(aTHX_ acc);
            } else if (ptype == AV_PACKED_NV) {
                const double *p = (const double *)AvPACKED_DATA(av);
                double res;
#ifdef PERL_FORCE_SCALAR
                res = perl_accel_scalar_sum_f64_cr(p, (size_t)n);
#else
                res = perl_accel_sum_f64_cr(p, (size_t)n);
#endif
                RETVAL = newSVnv(res);
            } else {
                const U8 *p = (const U8 *)AvPACKED_DATA(av);
                UV acc = 0;
                SSize_t i;
                for (i = 0; i < n; i++) acc += p[i];
                RETVAL = newSVuv(acc);
            }
        } else {
            /* Plain array: check if all IV or use exact NV summation */
            struct mt_shewchuk acc;
            SSize_t i;
            bool all_iv = TRUE;
            __int128 iacc = 0;
            bool has_pos_inf = FALSE, has_neg_inf = FALSE, has_nan = FALSE;
            bool all_neg_zero = TRUE;
            double nan_val = NAN;

            for (i = 0; i < n; i++) {
                SV **svp = av_fetch(av, i, 0);
                SV *sv = svp ? *svp : NULL;
                if (!sv || !SvOK(sv)) { all_iv = FALSE; break; }
                if ((SvFLAGS(sv) & (SVf_IOK|SVf_NOK|SVf_POK)) != SVf_IOK
                    || (SvIsUV(sv) && SvUVX(sv) > (UV)IV_MAX))
                {
                    all_iv = FALSE;
                    break;
                }
                iacc += (__int128)(SvIsUV(sv) ? (IV)SvUVX(sv) : SvIVX(sv));
            }
            if (all_iv) {
                RETVAL = mt_i128_to_sv(aTHX_ iacc);
            } else {
                shewchuk_init(&acc);
                for (i = 0; i < n; i++) {
                    SV **svp = av_fetch(av, i, 0);
                    double v = (svp && *svp) ? SvNV(*svp) : 0.0;
                    if (UNLIKELY(!isfinite(v))) {
                        if (isnan(v)) { has_nan = TRUE; nan_val = v; break; }
                        if (v > 0.0) has_pos_inf = TRUE;
                        else         has_neg_inf = TRUE;
                        all_neg_zero = FALSE;
                        continue;
                    }
                    if (v != 0.0 || !signbit(v)) all_neg_zero = FALSE;
                    shewchuk_grow(&acc, v);
                }
                if (has_nan)                         RETVAL = newSVnv(nan_val);
                else if (has_pos_inf && has_neg_inf) RETVAL = newSVnv(NAN);
                else if (has_pos_inf)                RETVAL = newSVnv(INFINITY);
                else if (has_neg_inf)                RETVAL = newSVnv(-INFINITY);
                else                                 RETVAL = newSVnv(shewchuk_round(&acc, all_neg_zero));
                shewchuk_free(&acc);
            }
        }
    OUTPUT:
        RETVAL

SV *
min(aref)
        SV *aref
    PREINIT:
        AV *av;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "min");
        n  = AvFILL(av) + 1;
        if (n <= 0) {
            RETVAL = &PL_sv_undef;
        } else if (SvPACKED_AV(av)) {
            U8 ptype = AvPACKED_TYPE(av);
            if (ptype == AV_PACKED_IV) {
                RETVAL = newSViv(mt_iv_min(aTHX_ (const IV *)AvPACKED_DATA(av), n));
            } else if (ptype == AV_PACKED_NV) {
                RETVAL = newSVnv(mt_nv_min(aTHX_ (const double *)AvPACKED_DATA(av), n));
            } else {
                const U8 *p = (const U8 *)AvPACKED_DATA(av);
                U8 m = p[0];
                SSize_t i;
                for (i = 1; i < n; i++) if (p[i] < m) m = p[i];
                RETVAL = newSViv((IV)m);
            }
        } else {
            SSize_t i;
            SV **svp = av_fetch(av, 0, 0);
            double m = (svp && *svp) ? SvNV(*svp) : 0.0;
            if (!isnan(m)) {
                for (i = 1; i < n; i++) {
                    double v;
                    svp = av_fetch(av, i, 0);
                    v = (svp && *svp) ? SvNV(*svp) : 0.0;
                    if (UNLIKELY(isnan(v))) { m = v; break; }
                    if (v < m) m = v;
                }
            }
            RETVAL = newSVnv(m);
        }
    OUTPUT:
        RETVAL

SV *
max(aref)
        SV *aref
    PREINIT:
        AV *av;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "max");
        n  = AvFILL(av) + 1;
        if (n <= 0) {
            RETVAL = &PL_sv_undef;
        } else if (SvPACKED_AV(av)) {
            U8 ptype = AvPACKED_TYPE(av);
            if (ptype == AV_PACKED_IV) {
                RETVAL = newSViv(mt_iv_max(aTHX_ (const IV *)AvPACKED_DATA(av), n));
            } else if (ptype == AV_PACKED_NV) {
                RETVAL = newSVnv(mt_nv_max(aTHX_ (const double *)AvPACKED_DATA(av), n));
            } else {
                const U8 *p = (const U8 *)AvPACKED_DATA(av);
                U8 m = p[0];
                SSize_t i;
                for (i = 1; i < n; i++) if (p[i] > m) m = p[i];
                RETVAL = newSViv((IV)m);
            }
        } else {
            SSize_t i;
            SV **svp = av_fetch(av, 0, 0);
            double m = (svp && *svp) ? SvNV(*svp) : 0.0;
            if (!isnan(m)) {
                for (i = 1; i < n; i++) {
                    double v;
                    svp = av_fetch(av, i, 0);
                    v = (svp && *svp) ? SvNV(*svp) : 0.0;
                    if (UNLIKELY(isnan(v))) { m = v; break; }
                    if (v > m) m = v;
                }
            }
            RETVAL = newSVnv(m);
        }
    OUTPUT:
        RETVAL

SV *
dot(aref, bref)
        SV *aref
        SV *bref
    PREINIT:
        AV *a, *b;
        SSize_t na, nb;
    CODE:
        a  = mt_expect_av(aTHX_ aref, "dot");
        b  = mt_expect_av(aTHX_ bref, "dot");
        na = AvFILL(a) + 1;
        nb = AvFILL(b) + 1;
        if (na != nb)
            Perl_croak(aTHX_ "MT::Array::dot: length mismatch (%" IVdf " vs %" IVdf ")",
                       (IV)na, (IV)nb);
        if (na <= 0) {
            RETVAL = newSViv(0);
        } else if (SvPACKED_AV(a) && SvPACKED_AV(b)
                   && AvPACKED_TYPE(a) == AvPACKED_TYPE(b))
        {
            U8 ptype = AvPACKED_TYPE(a);
            if (ptype == AV_PACKED_IV) {
                const IV *pa = (const IV *)AvPACKED_DATA(a);
                const IV *pb = (const IV *)AvPACKED_DATA(b);
                __int128 acc = 0;
                SSize_t i;
                for (i = 0; i < na; i++)
                    acc += (__int128)pa[i] * (__int128)pb[i];
                RETVAL = mt_i128_to_sv(aTHX_ acc);
            } else if (ptype == AV_PACKED_NV) {
                const double *pa = (const double *)AvPACKED_DATA(a);
                const double *pb = (const double *)AvPACKED_DATA(b);
                double res;
#ifdef PERL_FORCE_SCALAR
                res = perl_accel_scalar_dot_f64_cr(pa, pb, (size_t)na);
#else
                res = perl_accel_dot_f64_cr(pa, pb, (size_t)na);
#endif
                RETVAL = newSVnv(res);
            } else {
                const U8 *pa = (const U8 *)AvPACKED_DATA(a);
                const U8 *pb = (const U8 *)AvPACKED_DATA(b);
                UV acc = 0;
                SSize_t i;
                for (i = 0; i < na; i++)
                    acc += (UV)pa[i] * (UV)pb[i];
                RETVAL = newSVuv(acc);
            }
        } else {
            struct mt_shewchuk acc;
            SSize_t i;
            bool has_pos_inf = FALSE, has_neg_inf = FALSE, has_nan = FALSE;
            bool all_neg_zero = TRUE;
            double nan_val = NAN;

            shewchuk_init(&acc);
            for (i = 0; i < na; i++) {
                SV **sa = av_fetch(a, i, 0);
                SV **sb = av_fetch(b, i, 0);
                double da = (sa && *sa) ? SvNV(*sa) : 0.0;
                double db = (sb && *sb) ? SvNV(*sb) : 0.0;
                double prod, err;
                mt_two_prod(da, db, &prod, &err);
                if (UNLIKELY(!isfinite(prod))) {
                    if (isnan(prod)) { has_nan = TRUE; nan_val = prod; break; }
                    if (prod > 0.0) has_pos_inf = TRUE;
                    else            has_neg_inf = TRUE;
                    all_neg_zero = FALSE;
                    continue;
                }
                if (prod != 0.0 || !signbit(prod)) all_neg_zero = FALSE;
                shewchuk_grow(&acc, prod);
                if (err != 0.0 && isfinite(err)) shewchuk_grow(&acc, err);
            }
            if (has_nan)                         RETVAL = newSVnv(nan_val);
            else if (has_pos_inf && has_neg_inf) RETVAL = newSVnv(NAN);
            else if (has_pos_inf)                RETVAL = newSVnv(INFINITY);
            else if (has_neg_inf)                RETVAL = newSVnv(-INFINITY);
            else                                 RETVAL = newSVnv(shewchuk_round(&acc, all_neg_zero));
            shewchuk_free(&acc);
        }
    OUTPUT:
        RETVAL

SV *
map_add(aref, ksv)
        SV *aref
        SV *ksv
    PREINIT:
        AV *av, *out;
        SSize_t n, i;
    CODE:
        av = mt_expect_av(aTHX_ aref, "map_add");
        n  = AvFILL(av) + 1;
        SvGETMAGIC(ksv);
        if (SvPACKED_AV(av)) {
            U8 ptype = AvPACKED_TYPE(av);
            if (ptype == AV_PACKED_IV && (SvFLAGS(ksv) & (SVf_IOK|SVf_NOK|SVf_POK)) == SVf_IOK) {
                IV k = SvIV(ksv);
                const IV *src = (const IV *)AvPACKED_DATA(av);
                out = Perl_av_new_packed(aTHX_ AV_PACKED_IV, n);
                if (n > 0) {
                    IV *dst = (IV *)AvPACKED_DATA(out);
                    i = 0;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
                    if (PERL_SIMD_ON() && n >= 4) {
                        int64x2_t vk = vdupq_n_s64((int64_t)k);
                        for (; i + 1 < n; i += 2) {
                            int64x2_t v = vld1q_s64((const int64_t *)(src + i));
                            vst1q_s64((int64_t *)(dst + i), vaddq_s64(v, vk));
                        }
                    }
#endif
                    for (; i < n; i++) dst[i] = src[i] + k;
                    AvFILLp(out) = n - 1;
                }
                RETVAL = newRV_noinc((SV *)out);
            } else if (ptype == AV_PACKED_NV) {
                double k = SvNV(ksv);
                const double *src = (const double *)AvPACKED_DATA(av);
                out = Perl_av_new_packed(aTHX_ AV_PACKED_NV, n);
                if (n > 0) {
                    double *dst = (double *)AvPACKED_DATA(out);
                    i = 0;
#if !defined(PERL_FORCE_SCALAR) && defined(__aarch64__)
                    if (PERL_SIMD_ON() && n >= 4) {
                        float64x2_t vk = vdupq_n_f64(k);
                        for (; i + 1 < n; i += 2) {
                            float64x2_t v = vld1q_f64(src + i);
                            vst1q_f64(dst + i, vaddq_f64(v, vk));
                        }
                    }
#endif
                    for (; i < n; i++) dst[i] = src[i] + k;
                    AvFILLp(out) = n - 1;
                }
                RETVAL = newRV_noinc((SV *)out);
            } else {
                double k = SvNV(ksv);
                out = Perl_av_new_packed(aTHX_ AV_PACKED_NV, n);
                if (n > 0) {
                    double *dst = (double *)AvPACKED_DATA(out);
                    if (ptype == AV_PACKED_IV) {
                        const IV *src = (const IV *)AvPACKED_DATA(av);
                        for (i = 0; i < n; i++) dst[i] = (double)src[i] + k;
                    } else {
                        const U8 *src = (const U8 *)AvPACKED_DATA(av);
                        for (i = 0; i < n; i++) dst[i] = (double)src[i] + k;
                    }
                    AvFILLp(out) = n - 1;
                }
                RETVAL = newRV_noinc((SV *)out);
            }
        } else {
            double k = SvNV(ksv);
            out = newAV();
            if (n > 0) av_extend(out, n - 1);
            for (i = 0; i < n; i++) {
                SV **svp = av_fetch(av, i, 0);
                double v = (svp && *svp) ? SvNV(*svp) : 0.0;
                av_push(out, newSVnv(v + k));
            }
            RETVAL = newRV_noinc((SV *)out);
        }
    OUTPUT:
        RETVAL

SV *
slice(aref, start, len)
        SV *aref
        IV start
        IV len
    PREINIT:
        AV *av, *out;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "slice");
        n  = AvFILL(av) + 1;
        if (start < 0) start += n;
        if (start < 0) start = 0;
        if (len < 0 || start >= n) len = 0;
        if (start + len > n) len = n - start;
        if (SvPACKED_AV(av)) {
            U8 ptype = AvPACKED_TYPE(av);
            out = Perl_av_new_packed(aTHX_ ptype, (SSize_t)len);
            if (len > 0) {
                size_t esz = (ptype == AV_PACKED_IV) ? sizeof(IV)
                           : (ptype == AV_PACKED_NV) ? sizeof(NV) : sizeof(U8);
                Copy((const char *)AvPACKED_DATA(av) + (Size_t)start * esz,
                     (char *)AvPACKED_DATA(out),
                     (Size_t)len * esz, char);
                AvFILLp(out) = (SSize_t)len - 1;
            }
        } else {
            SSize_t i;
            out = newAV();
            if (len > 0) av_extend(out, (SSize_t)len - 1);
            for (i = 0; i < (SSize_t)len; i++) {
                SV **svp = av_fetch(av, (SSize_t)start + i, 0);
                av_push(out, (svp && *svp) ? newSVsv(*svp) : newSV(0));
            }
        }
        RETVAL = newRV_noinc((SV *)out);
    OUTPUT:
        RETVAL

UV
count(aref, byte_iv)
        SV *aref
        IV byte_iv
    PREINIT:
        AV *av;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "count");
        if (byte_iv < 0 || byte_iv > 255)
            Perl_croak(aTHX_ "MT::Array::count: byte value %" IVdf " out of 0..255", byte_iv);
        n = AvFILL(av) + 1;
        if (n <= 0) {
            RETVAL = 0;
        } else if (SvPACKED_AV(av) && AvPACKED_TYPE(av) == AV_PACKED_U8) {
            RETVAL = mt_u8_count(aTHX_ (const U8 *)AvPACKED_DATA(av), n, (U8)byte_iv);
        } else {
            SSize_t i;
            UV cnt = 0;
            for (i = 0; i < n; i++) {
                SV **svp = av_fetch(av, i, 0);
                if (svp && *svp && SvOK(*svp) && SvIV(*svp) == byte_iv)
                    cnt++;
            }
            RETVAL = cnt;
        }
    OUTPUT:
        RETVAL

IV
find(aref, byte_iv)
        SV *aref
        IV byte_iv
    PREINIT:
        AV *av;
        SSize_t n;
    CODE:
        av = mt_expect_av(aTHX_ aref, "find");
        if (byte_iv < 0 || byte_iv > 255)
            Perl_croak(aTHX_ "MT::Array::find: byte value %" IVdf " out of 0..255", byte_iv);
        n = AvFILL(av) + 1;
        if (n <= 0) {
            RETVAL = -1;
        } else if (SvPACKED_AV(av) && AvPACKED_TYPE(av) == AV_PACKED_U8) {
            const U8 *p = (const U8 *)AvPACKED_DATA(av);
            STRLEN pos = PERL_SIMD_ON()
                ? perl_simd_span_not_byte(p, (STRLEN)n, (U8)byte_iv)
                : perl_simd_span_not_byte_scalar(p, (STRLEN)n, (U8)byte_iv);
            RETVAL = (pos < (STRLEN)n) ? (IV)pos : -1;
        } else {
            SSize_t i;
            RETVAL = -1;
            for (i = 0; i < n; i++) {
                SV **svp = av_fetch(av, i, 0);
                if (svp && *svp && SvOK(*svp) && SvIV(*svp) == byte_iv) {
                    RETVAL = (IV)i;
                    break;
                }
            }
        }
    OUTPUT:
        RETVAL

SV *
vec_add(aref, bref)
        SV *aref
        SV *bref
    PREINIT:
        AV *a, *b, *out;
        SSize_t na, nb, i;
    CODE:
        a  = mt_expect_av(aTHX_ aref, "vec_add");
        b  = mt_expect_av(aTHX_ bref, "vec_add");
        na = AvFILL(a) + 1;
        nb = AvFILL(b) + 1;
        if (na != nb)
            Perl_croak(aTHX_ "MT::Array::vec_add: length mismatch (%" IVdf " vs %" IVdf ")",
                       (IV)na, (IV)nb);
        out = Perl_av_new_packed(aTHX_ AV_PACKED_NV, na);
        if (na > 0) {
            double *dst = (double *)AvPACKED_DATA(out);
            if (SvPACKED_AV(a) && SvPACKED_AV(b)
                && AvPACKED_TYPE(a) == AV_PACKED_NV
                && AvPACKED_TYPE(b) == AV_PACKED_NV)
            {
                const double *pa = (const double *)AvPACKED_DATA(a);
                const double *pb = (const double *)AvPACKED_DATA(b);
#ifdef PERL_FORCE_SCALAR
                perl_accel_scalar_vec_add_f64(pa, pb, dst, (size_t)na);
#else
                perl_accel_vec_add_f64(pa, pb, dst, (size_t)na);
#endif
            } else {
                for (i = 0; i < na; i++) {
                    SV **sa = av_fetch(a, i, 0);
                    SV **sb = av_fetch(b, i, 0);
                    double da = (sa && *sa) ? SvNV(*sa) : 0.0;
                    double db = (sb && *sb) ? SvNV(*sb) : 0.0;
                    dst[i] = da + db;
                }
            }
            AvFILLp(out) = na - 1;
        }
        RETVAL = newRV_noinc((SV *)out);
    OUTPUT:
        RETVAL

SV *
vec_mul(aref, bref)
        SV *aref
        SV *bref
    PREINIT:
        AV *a, *b, *out;
        SSize_t na, nb, i;
    CODE:
        a  = mt_expect_av(aTHX_ aref, "vec_mul");
        b  = mt_expect_av(aTHX_ bref, "vec_mul");
        na = AvFILL(a) + 1;
        nb = AvFILL(b) + 1;
        if (na != nb)
            Perl_croak(aTHX_ "MT::Array::vec_mul: length mismatch (%" IVdf " vs %" IVdf ")",
                       (IV)na, (IV)nb);
        out = Perl_av_new_packed(aTHX_ AV_PACKED_NV, na);
        if (na > 0) {
            double *dst = (double *)AvPACKED_DATA(out);
            if (SvPACKED_AV(a) && SvPACKED_AV(b)
                && AvPACKED_TYPE(a) == AV_PACKED_NV
                && AvPACKED_TYPE(b) == AV_PACKED_NV)
            {
                const double *pa = (const double *)AvPACKED_DATA(a);
                const double *pb = (const double *)AvPACKED_DATA(b);
#ifdef PERL_FORCE_SCALAR
                perl_accel_scalar_vec_mul_f64(pa, pb, dst, (size_t)na);
#else
                perl_accel_vec_mul_f64(pa, pb, dst, (size_t)na);
#endif
            } else {
                for (i = 0; i < na; i++) {
                    SV **sa = av_fetch(a, i, 0);
                    SV **sb = av_fetch(b, i, 0);
                    double da = (sa && *sa) ? SvNV(*sa) : 0.0;
                    double db = (sb && *sb) ? SvNV(*sb) : 0.0;
                    dst[i] = da * db;
                }
            }
            AvFILLp(out) = na - 1;
        }
        RETVAL = newRV_noinc((SV *)out);
    OUTPUT:
        RETVAL

SV *
axpy(alpha_sv, xref, yref)
        SV *alpha_sv
        SV *xref
        SV *yref
    PREINIT:
        AV *x, *y, *out;
        SSize_t nx, ny, i;
        double alpha;
    CODE:
        SvGETMAGIC(alpha_sv);
        alpha = SvNV(alpha_sv);
        x  = mt_expect_av(aTHX_ xref, "axpy");
        y  = mt_expect_av(aTHX_ yref, "axpy");
        nx = AvFILL(x) + 1;
        ny = AvFILL(y) + 1;
        if (nx != ny)
            Perl_croak(aTHX_ "MT::Array::axpy: length mismatch (%" IVdf " vs %" IVdf ")",
                       (IV)nx, (IV)ny);
        out = Perl_av_new_packed(aTHX_ AV_PACKED_NV, nx);
        if (nx > 0) {
            double *dst = (double *)AvPACKED_DATA(out);
            if (SvPACKED_AV(x) && SvPACKED_AV(y)
                && AvPACKED_TYPE(x) == AV_PACKED_NV
                && AvPACKED_TYPE(y) == AV_PACKED_NV)
            {
                const double *px = (const double *)AvPACKED_DATA(x);
                const double *py = (const double *)AvPACKED_DATA(y);
#ifdef PERL_FORCE_SCALAR
                perl_accel_scalar_vec_axpy_f64(alpha, px, py, dst, (size_t)nx);
#else
                perl_accel_vec_axpy_f64(alpha, px, py, dst, (size_t)nx);
#endif
            } else {
                for (i = 0; i < nx; i++) {
                    SV **sx = av_fetch(x, i, 0);
                    SV **sy = av_fetch(y, i, 0);
                    double dx = (sx && *sx) ? SvNV(*sx) : 0.0;
                    double dy = (sy && *sy) ? SvNV(*sy) : 0.0;
                    double prod = alpha * dx;
                    dst[i] = prod + dy;
                }
            }
            AvFILLp(out) = nx - 1;
        }
        RETVAL = newRV_noinc((SV *)out);
    OUTPUT:
        RETVAL

IV
dot_i8(aref, bref)
        SV *aref
        SV *bref
    PREINIT:
        AV *a, *b;
        SSize_t na, nb;
        int32_t res = 0;
    CODE:
        a  = mt_expect_av(aTHX_ aref, "dot_i8");
        b  = mt_expect_av(aTHX_ bref, "dot_i8");
        na = AvFILL(a) + 1;
        nb = AvFILL(b) + 1;
        if (na != nb)
            Perl_croak(aTHX_ "MT::Array::dot_i8: length mismatch (%" IVdf " vs %" IVdf ")",
                       (IV)na, (IV)nb);
        if (na > 0) {
            if (SvPACKED_AV(a) && SvPACKED_AV(b)
                && AvPACKED_TYPE(a) == AV_PACKED_U8
                && AvPACKED_TYPE(b) == AV_PACKED_U8)
            {
                const int8_t *pa = (const int8_t *)AvPACKED_DATA(a);
                const int8_t *pb = (const int8_t *)AvPACKED_DATA(b);
#ifdef PERL_FORCE_SCALAR
                res = perl_accel_scalar_dot_i8_i32(pa, pb, (size_t)na);
#else
                res = perl_accel_dot_i8_i32(pa, pb, (size_t)na);
#endif
            } else {
                int8_t sbuf_a[256], sbuf_b[256];
                int8_t *pa = sbuf_a;
                int8_t *pb = sbuf_b;
                bool heap = (na > 256);
                if (heap) {
                    Newx(pa, na, int8_t);
                    Newx(pb, na, int8_t);
                }
                mt_av_to_i8(aTHX_ a, na, pa, "dot_i8");
                mt_av_to_i8(aTHX_ b, nb, pb, "dot_i8");
#ifdef PERL_FORCE_SCALAR
                res = perl_accel_scalar_dot_i8_i32(pa, pb, (size_t)na);
#else
                res = perl_accel_dot_i8_i32(pa, pb, (size_t)na);
#endif
                if (heap) {
                    Safefree(pa);
                    Safefree(pb);
                }
            }
        }
        RETVAL = (IV)res;
    OUTPUT:
        RETVAL

MODULE = MT		PACKAGE = MT::Accel

PROTOTYPES: DISABLE

SV *
backends()
    PREINIT:
        const perl_accel_backend_t *list[32];
        const perl_accel_backend_t *act;
        size_t count, i;
        AV *av;
    CODE:
        act = perl_accel_active();
        count = perl_accel_list(list, 32);
        if (count > 32)
            count = 32;
        av = newAV();
        av_extend(av, (SSize_t)count);
        for (i = 0; i < count; i++) {
            const perl_accel_backend_t *b = list[i];
            HV *hv = newHV();
            (void)hv_stores(hv, "name",
                            newSVpv(b->name ? b->name : "", 0));
            (void)hv_stores(hv, "description",
                            newSVpv(b->description ? b->description : "", 0));
            (void)hv_stores(hv, "device_class",
                            newSVpv(mt_accel_device_class_name(b->device_class), 0));
            (void)hv_stores(hv, "priority",
                            newSViv((IV)b->priority));
            (void)hv_stores(hv, "available",
                            newSViv(perl_accel_backend_available(b->name) ? 1 : 0));
            (void)hv_stores(hv, "active",
                            newSViv((b == act) ? 1 : 0));
            (void)hv_stores(hv, "caps",
                            newSVuv((UV)b->caps));
            (void)hv_stores(hv, "min_len_f64",
                            newSVuv((UV)b->min_len_f64));
            (void)hv_stores(hv, "min_len_i8",
                            newSVuv((UV)b->min_len_i8));
            (void)hv_stores(hv, "min_len_nx",
                            newSVuv((UV)b->min_len_nx));
            av_push(av, newRV_noinc((SV *)hv));
        }
        RETVAL = newRV_noinc((SV *)av);
    OUTPUT:
        RETVAL

SV *
active()
    PREINIT:
        const perl_accel_backend_t *act;
    CODE:
        act = perl_accel_active();
        RETVAL = newSVpv((act && act->name) ? act->name : "scalar", 0);
    OUTPUT:
        RETVAL

bool
enabled()
    CODE:
        RETVAL = perl_accel_is_enabled() ? TRUE : FALSE;
    OUTPUT:
        RETVAL

SV *
simd_level()
    PREINIT:
        const char *name;
    CODE:
        name = Perl_simd_level_name(Perl_simd_extended_level());
        RETVAL = newSVpv(name ? name : "scalar", 0);
    OUTPUT:
        RETVAL

SV *
simd_caps()
    PREINIT:
        uint64_t caps;
        HV *hv;
    CODE:
        caps = Perl_simd_caps();
        hv = newHV();
        (void)hv_stores(hv, "bits",         newSVuv((UV)caps));
        (void)hv_stores(hv, "sse2",         newSViv((caps & PERL_SIMD_CAP_SSE2) ? 1 : 0));
        (void)hv_stores(hv, "avx2",         newSViv((caps & PERL_SIMD_CAP_AVX2) ? 1 : 0));
        (void)hv_stores(hv, "avx512f",      newSViv((caps & PERL_SIMD_CAP_AVX512F) ? 1 : 0));
        (void)hv_stores(hv, "avx512bw",     newSViv((caps & PERL_SIMD_CAP_AVX512BW) ? 1 : 0));
        (void)hv_stores(hv, "avx512vl",     newSViv((caps & PERL_SIMD_CAP_AVX512VL) ? 1 : 0));
        (void)hv_stores(hv, "avx_vnni",     newSViv((caps & PERL_SIMD_CAP_AVX_VNNI) ? 1 : 0));
        (void)hv_stores(hv, "avx512_vnni",  newSViv((caps & PERL_SIMD_CAP_AVX512_VNNI) ? 1 : 0));
        (void)hv_stores(hv, "neon",         newSViv((caps & PERL_SIMD_CAP_NEON) ? 1 : 0));
        (void)hv_stores(hv, "neon_dotprod", newSViv((caps & PERL_SIMD_CAP_NEON_DOTPROD) ? 1 : 0));
        (void)hv_stores(hv, "neon_i8mm",    newSViv((caps & PERL_SIMD_CAP_NEON_I8MM) ? 1 : 0));
        (void)hv_stores(hv, "apple_amx",    newSViv((caps & PERL_SIMD_CAP_APPLE_AMX) ? 1 : 0));
        (void)hv_stores(hv, "vsx",          newSViv((caps & PERL_SIMD_CAP_VSX) ? 1 : 0));
        (void)hv_stores(hv, "vsx2",         newSViv((caps & PERL_SIMD_CAP_VSX2) ? 1 : 0));
        (void)hv_stores(hv, "vsx3",         newSViv((caps & PERL_SIMD_CAP_VSX3) ? 1 : 0));
        (void)hv_stores(hv, "s390x_vx",     newSViv((caps & PERL_SIMD_CAP_S390X_VX) ? 1 : 0));
        (void)hv_stores(hv, "s390x_nnpa",   newSViv((caps & PERL_SIMD_CAP_S390X_NNPA) ? 1 : 0));
        RETVAL = newRV_noinc((SV *)hv);
    OUTPUT:
        RETVAL

SV *
stats()
    PREINIT:
        perl_accel_stats_t st;
        HV *hv;
    CODE:
        perl_accel_get_stats(&st);
        hv = newHV();
        (void)hv_stores(hv, "offload_calls",  newSVuv((UV)st.offload_calls));
        (void)hv_stores(hv, "fallback_calls", newSVuv((UV)st.fallback_calls));
        (void)hv_stores(hv, "last_backend",
                        st.last_backend_name
                            ? newSVpv(st.last_backend_name, 0)
                            : newSV(0));
        RETVAL = newRV_noinc((SV *)hv);
    OUTPUT:
        RETVAL

void
reset_stats()
    CODE:
        perl_accel_reset_stats();

INCLUDE: mt_phase6.xs.inc
INCLUDE: mt_phase10.xs.inc

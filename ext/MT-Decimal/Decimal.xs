/*
 * MT::Decimal XS bindings (threadsafe, PERL_NO_GET_CONTEXT).
 */

#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "mt_decimal.h"

static SV *
S_wrap_dec(pTHX_ mt_dec128_t d, const char *pkg)
{
    SV *inner = newSVpvn((const char *)&d, sizeof(d));
    SV *obj = newRV_noinc(inner);
    sv_bless(obj, gv_stashpv(pkg ? pkg : "MT::Decimal", GV_ADD));
    return obj;
}

static mt_dec128_t
S_sv_to_dec(pTHX_ SV *sv, int rmode)
{
    if (sv_isobject(sv) && sv_derived_from(sv, "MT::Decimal")) {
        SV *inner = SvRV(sv);
        STRLEN len = 0;
        const char *p = SvPV_const(inner, len);
        if (len == sizeof(mt_dec128_t)) {
            mt_dec128_t d;
            memcpy(&d, p, sizeof(d));
            return d;
        }
    }
    if (SvIOK(sv) && !SvIsUV(sv)) {
        return mt_dec_from_i64((int64_t)SvIV(sv));
    }
    if (SvNOK(sv) && !SvPOK(sv)) {
        return mt_dec_from_double((double)SvNV(sv), rmode);
    }
    {
        STRLEN len = 0;
        const char *s = SvPV_const(sv, len);
        return mt_dec_from_string(s, (size_t)len, rmode);
    }
}

MODULE = MT::Decimal    PACKAGE = MT::Decimal

PROTOTYPES: DISABLE

const char *
backend()
  CODE:
    RETVAL = mt_dec_backend();
  OUTPUT:
    RETVAL

SV *
new(klass, val, rmode = MT_DEC_ROUND_HALF_EVEN)
    const char *klass
    SV *val
    int rmode
  CODE:
    mt_dec128_t d = S_sv_to_dec(aTHX_ val, rmode);
    RETVAL = S_wrap_dec(aTHX_ d, klass);
  OUTPUT:
    RETVAL

SV *
add(a, b, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    SV *b
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_add(da, db, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
sub(a, b, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    SV *b
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_sub(da, db, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
mul(a, b, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    SV *b
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_mul(da, db, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
div(a, b, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    SV *b
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_div(da, db, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
mod(a, b, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    SV *b
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_rem(da, db, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
neg(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_neg(da), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
abs(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_abs(da), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
quantize(a, exp, rmode = MT_DEC_ROUND_HALF_EVEN)
    SV *a
    IV exp
    int rmode
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, rmode);
    RETVAL = S_wrap_dec(aTHX_ mt_dec_quantize(da, (int32_t)exp, rmode), "MT::Decimal");
  OUTPUT:
    RETVAL

SV *
cmp(a, b)
    SV *a
    SV *b
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    mt_dec128_t db = S_sv_to_dec(aTHX_ b, MT_DEC_ROUND_HALF_EVEN);
    int c = mt_dec_cmp(da, db);
    if (c == 2)
        RETVAL = &PL_sv_undef;
    else
        RETVAL = newSViv(c);
  OUTPUT:
    RETVAL

SV *
to_string(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    char buf[80];
    size_t n = mt_dec_to_string(da, buf, sizeof(buf));
    RETVAL = newSVpvn(buf, n);
  OUTPUT:
    RETVAL

NV
to_nv(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (NV)mt_dec_to_double(da);
  OUTPUT:
    RETVAL

SV *
to_bytes(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    uint8_t out[16];
    mt_dec_to_bid128(da, out);
    RETVAL = newSVpvn((const char *)out, 16);
  OUTPUT:
    RETVAL

bool
is_nan(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (da.kind == MT_DEC_FLAG_NAN);
  OUTPUT:
    RETVAL

bool
is_inf(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (da.kind == MT_DEC_FLAG_INF);
  OUTPUT:
    RETVAL

bool
is_zero(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (da.kind == MT_DEC_FLAG_FINITE && da.lo == 0 && da.hi == 0);
  OUTPUT:
    RETVAL

bool
is_finite(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (da.kind == MT_DEC_FLAG_FINITE);
  OUTPUT:
    RETVAL

bool
is_neg(a)
    SV *a
  CODE:
    mt_dec128_t da = S_sv_to_dec(aTHX_ a, MT_DEC_ROUND_HALF_EVEN);
    RETVAL = (da.kind != MT_DEC_FLAG_NAN && da.sign != 0);
  OUTPUT:
    RETVAL

/*    mt_xs_bridge.c
 *
 *    perl5-mt: the bridge between a host interpreter (a worker, or
 *    the main interpreter) and an XS compartment: the TLV marshalling of
 *    values in both directions, the host stub call (Perl_mt_xs_remote_call,
 *    reached from Perl_rpp_invoke_xs), the service of CALL, METHOD, TIE_*
 *    and variable frames inside the compartment, the reverse trampoline
 *    through which XS code in the compartment calls host code refs, the
 *    tied shapes of remote hashes and arrays, and the MT::XS::Proxy
 *    classes.  The pool, rings and slots are in mt_xs_compartment.c.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_XS_BRIDGE_C
#include "perl.h"
#include "XSUB.h"
#include "mt_xs.h"
#include "mt_fiber.h"
#include "mt_cap.h"

/* ---- the TLV codec --------------------------------------------------------- */

/*
 * A value is one tag byte and its body:
 *
 *   u                         undef
 *   b  u8                     one of the two boolean immortals
 *   i  i64 / U  u64 / n  NV   numbers
 *   d  kind, number, u8 utf8, u32 len, bytes   a dualvar (both halves)
 *   p  u8 utf8, u32 len, bytes          a string up to MT_XS_INLINE_PV
 *   P  u8 utf8, u32 shm, u64 off, u64 len  a longer string, placed in the
 *                                        slot's arena (shm 0) by itself
 *   R  value                  reference to a scalar
 *   A  u32 n, n values        array (deep copy)
 *   H  u32 n, n x (u8 utf8, u32 klen, key, value)   hash (deep copy)
 *   B  u16 clen, class, value blessed: the referent, deep copied
 *   C  u32 slot, u32 id       a host code ref (the host keeps it pinned)
 *   h  u16 comp, u16 gen, u64 ptr, u8 reftype, u8 proxy, u16 clen, class
 *                             a remote handle: an SV pinned in a compartment
 *
 * The encoder knows which side it is on: the host turns its code refs into
 * C and its remote handles back into h; the compartment turns blessed
 * values and code refs into h (pinning them) and a trampoline back into C.
 */

enum {
    MT_XS_SIDE_HOST = 0,
    MT_XS_SIDE_COMP = 1
};

#define MT_XS_ENC_DEEP_BLESSED  0x1u   /* exceptions: copy objects instead */
#define MT_XS_ENC_PROXY         0x2u   /* handles made are proxy objects    */

typedef struct mt_xs_enc {
    SV      *buf;            /* the stream being built                     */
    U32      slot;           /* the arena for P strings                    */
    int      side;
    U32      flags;
    U32      depth;
} mt_xs_enc_t;

typedef struct mt_xs_dec {
    const char *p;
    const char *end;
    U32         slot;
    int         side;
    U32         flags;
    U32         comp;        /* host: the compartment the stream came from */
    U32         gen;
} mt_xs_dec_t;

static void
S_enc_init(pTHX_ mt_xs_enc_t *e, U32 slot, int side, U32 flags)
{
    e->buf = sv_2mortal(newSV(256));
    SvPOK_only(e->buf);
    SvCUR_set(e->buf, 0);
    e->slot = slot;
    e->side = side;
    e->flags = flags;
    e->depth = 0;
}

#define ENC_RAW(e, ptr, len) sv_catpvn_nomg((e)->buf, (const char *)(ptr), (len))
#define ENC_U8(e, v)   STMT_START { U8 v_ = (U8)(v); ENC_RAW(e, &v_, 1); } STMT_END
#define ENC_U16(e, v)  STMT_START { U16 v_ = (U16)(v); ENC_RAW(e, &v_, 2); } STMT_END
#define ENC_U32(e, v)  STMT_START { U32 v_ = (U32)(v); ENC_RAW(e, &v_, 4); } STMT_END
#define ENC_U64(e, v)  STMT_START { U64 v_ = (U64)(v); ENC_RAW(e, &v_, 8); } STMT_END

static void S_enc_sv(pTHX_ mt_xs_enc_t *e, SV *sv);

static void
S_enc_pv(pTHX_ mt_xs_enc_t *e, const char *pv, STRLEN len, bool utf8)
{
    if (len <= MT_XS_INLINE_PV) {
        ENC_U8(e, 'p');
        ENC_U8(e, utf8);
        ENC_U32(e, len);
        ENC_RAW(e, pv, len);
    }
    else {
        /* the shm path: the bytes go to the arena by themselves and the
         * stream carries (shm, offset, length) */
        U64 off;
        char * const dst = (char *)Perl_mt_xs_arena_alloc(aTHX_ e->slot, len, &off);
        Copy(pv, dst, len, char);
        ENC_U8(e, 'P');
        ENC_U8(e, utf8);
        ENC_U32(e, 0);
        ENC_U64(e, off);
        ENC_U64(e, len);
    }
}

static void
S_enc_handle(pTHX_ mt_xs_enc_t *e, U32 comp, U32 gen, U64 ptr, U8 reftype,
             bool proxy, const char *rclass, STRLEN clen)
{
    PERL_UNUSED_CONTEXT;
    ENC_U8(e, 'h');
    ENC_U16(e, comp);
    ENC_U16(e, gen);
    ENC_U64(e, ptr);
    ENC_U8(e, reftype);
    ENC_U8(e, proxy);
    ENC_U16(e, clen);
    ENC_RAW(e, rclass, clen);
}

/* compartment: pin sv and describe it to the host */
static void
S_enc_export(pTHX_ mt_xs_enc_t *e, SV *referent, HV *stash)
{
    const U64 ptr = Perl_mt_xs_export_sv(aTHX_ referent, FALSE);
    const char *cls = "";
    STRLEN clen = 0;
    U8 reftype = 0;
    if (stash && HvNAME_HEK(stash)) {
        cls = HvNAME_get(stash);
        clen = HvNAMELEN_get(stash);
    }
    switch (SvTYPE(referent)) {
    case SVt_PVHV: reftype = SVt_PVHV; break;
    case SVt_PVAV: reftype = SVt_PVAV; break;
    case SVt_PVCV: reftype = SVt_PVCV; break;
    default: break;
    }
    S_enc_handle(aTHX_ e, Perl_mt_xs_my_comp(), Perl_mt_xs_my_gen(), ptr, reftype,
                 cBOOL(e->flags & MT_XS_ENC_PROXY), cls, clen);
}

/* the trampoline CVs the compartment makes for host code refs carry
 * this in their any_sv (and a magic that releases the host's pin) */
typedef struct mt_xs_tramp {
    U32 slot;
    U32 id;
} mt_xs_tramp_t;

static void S_tramp_xsub(pTHX_ CV *cv);

static bool
S_is_tramp(CV *cv)
{
    return CvISXSUB(cv) && CvXSUB(cv) == S_tramp_xsub;
}

/* a host hash or array shaped as a tie: the tie object is a remote handle */
static perl_mt_cap_ref_t *
S_tied_shape_ref(pTHX_ SV *referent)
{
    MAGIC *mg;
    if ((SvTYPE(referent) != SVt_PVHV && SvTYPE(referent) != SVt_PVAV)
        || !SvRMAGICAL(referent)
        || !(mg = mg_find(referent, PERL_MAGIC_tied)) || !mg->mg_obj)
        return NULL;
    return Perl_mt_xs_handle_ref(aTHX_ mg->mg_obj);
}

static void
S_enc_ref(pTHX_ mt_xs_enc_t *e, SV *rv)
{
    SV * const referent = SvRV(rv);
    HV * const stash = SvOBJECT(referent) ? SvSTASH(referent) : NULL;

    if (++e->depth > MT_XS_MAX_DEPTH)
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);

    if (e->side == MT_XS_SIDE_HOST) {
        perl_mt_cap_ref_t *ref;
        if ((ref = Perl_mt_xs_handle_ref(aTHX_ rv))
            || (ref = S_tied_shape_ref(aTHX_ referent))) {
            if (!ref->ptr)
                Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
            S_enc_handle(aTHX_ e, ref->comp, ref->cgen, ref->ptr, ref->reftype,
                         ref->proxy, ref->rclass ? ref->rclass : "",
                         ref->rclass ? strlen(ref->rclass) : 0);
            e->depth--;
            return;
        }
        if (Perl_mt_cap_is_handle(aTHX_ rv))
            Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
        if (SvTYPE(referent) == SVt_PVCV) {
            /* a host code ref: pinned here, called back from there */
            AV *hh = PL_mt_xs_hh;
            if (!hh)
                hh = PL_mt_xs_hh = newAV();
            av_push(hh, newRV_inc(referent));
            ENC_U8(e, 'C');
            ENC_U32(e, e->slot);
            ENC_U32(e, av_top_index(hh));
            e->depth--;
            return;
        }
    }
    else {
        /* the compartment: blessed values and code live here, the host
         * gets handles; a trampoline goes back as the host's own code */
        if (SvTYPE(referent) == SVt_PVCV && S_is_tramp((CV *)referent)) {
            const mt_xs_tramp_t * const t =
                (const mt_xs_tramp_t *)SvPVX(CvXSUBANY(referent).any_sv);
            ENC_U8(e, 'C');
            ENC_U32(e, t->slot);
            ENC_U32(e, t->id);
            e->depth--;
            return;
        }
        if (SvTYPE(referent) == SVt_PVCV
            || (stash && !(e->flags & MT_XS_ENC_DEEP_BLESSED))) {
            S_enc_export(aTHX_ e, referent, stash);
            e->depth--;
            return;
        }
    }

    if (stash) {
        ENC_U8(e, 'B');
        ENC_U16(e, HvNAMELEN(stash));
        ENC_RAW(e, HvNAME(stash), HvNAMELEN(stash));
    }
    switch (SvTYPE(referent)) {
    case SVt_PVAV: {
        AV * const av = (AV *)referent;
        const SSize_t n = av_count(av);
        SSize_t i;
        ENC_U8(e, 'A');
        ENC_U32(e, n);
        for (i = 0; i < n; i++) {
            SV ** const svp = av_fetch(av, i, 0);
            S_enc_sv(aTHX_ e, svp ? *svp : &PL_sv_undef);
        }
        break;
    }
    case SVt_PVHV: {
        HV * const hv = (HV *)referent;
        HE *he;
        STRLEN mark;
        U32 n = 0;
        ENC_U8(e, 'H');
        mark = SvCUR(e->buf);
        ENC_U32(e, 0);
        hv_iterinit(hv);
        while ((he = hv_iternext(hv))) {
            SV * const val = hv_iterval(hv, he);
            const char *k;
            STRLEN klen;
            bool kutf8 = FALSE;
            if (HeKLEN(he) == HEf_SVKEY) {
                SV * const ksv = HeKEY_sv(he);
                k = SvPV(ksv, klen);
                kutf8 = cBOOL(SvUTF8(ksv));
            }
            else {
                k = HeKEY(he);
                klen = HeKLEN(he);
                kutf8 = cBOOL(HeKUTF8(he));
            }
            ENC_U8(e, kutf8);
            ENC_U32(e, klen);
            ENC_RAW(e, k, klen);
            S_enc_sv(aTHX_ e, val);
            n++;
        }
        Copy(&n, SvPVX(e->buf) + mark, 4, char);
        break;
    }
    case SVt_PVCV:
    case SVt_PVGV:
    case SVt_PVIO:
    case SVt_PVFM:
    case SVt_REGEXP:
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    default:
        if (isGV_with_GP(referent))
            Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
        ENC_U8(e, 'R');
        S_enc_sv(aTHX_ e, referent);
        break;
    }
    e->depth--;
}

static void
S_enc_sv(pTHX_ mt_xs_enc_t *e, SV *sv)
{
    if (!sv) {
        ENC_U8(e, 'u');
        return;
    }
    SvGETMAGIC(sv);
    if (sv == &PL_sv_yes || sv == &PL_sv_no) {
        ENC_U8(e, 'b');
        ENC_U8(e, sv == &PL_sv_yes);
        return;
    }
    if (SvROK(sv)) {
        S_enc_ref(aTHX_ e, sv);
        return;
    }
    if (isGV_with_GP(sv) || SvTYPE(sv) == SVt_PVIO)
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    if (!SvOK(sv)) {
        ENC_U8(e, 'u');
        return;
    }
    if (SvPOK(sv) && (SvIOK(sv) || SvNOK(sv))) {
        /* a dualvar (Compress::Raw::Zlib's status codes, $!): both halves */
        STRLEN len;
        const char * const pv = SvPV_nomg(sv, len);
        ENC_U8(e, 'd');
        if (SvNOK(sv) && !SvIOK(sv)) {
            const NV nv = SvNVX(sv);
            ENC_U8(e, 'n');
            ENC_RAW(e, &nv, sizeof nv);
        }
        else {
            ENC_U8(e, SvIsUV(sv) ? 'U' : 'i');
            ENC_U64(e, SvIsUV(sv) ? (U64)SvUVX(sv) : (U64)SvIVX(sv));
        }
        ENC_U8(e, cBOOL(SvUTF8(sv)));
        ENC_U32(e, len);
        ENC_RAW(e, pv, len);
        return;
    }
    if (SvIOK(sv)) {
        if (SvIsUV(sv)) {
            ENC_U8(e, 'U');
            ENC_U64(e, SvUVX(sv));
        }
        else {
            ENC_U8(e, 'i');
            ENC_U64(e, (U64)SvIVX(sv));
        }
        return;
    }
    if (SvNOK(sv)) {
        const NV nv = SvNVX(sv);
        ENC_U8(e, 'n');
        ENC_RAW(e, &nv, sizeof nv);
        return;
    }
    {
        STRLEN len;
        const char * const pv = SvPV_nomg(sv, len);
        S_enc_pv(aTHX_ e, pv, len, cBOOL(SvUTF8(sv)));
    }
}

/* ---- decoding ---------------------------------------------------------------- */

static void
S_dec_need(pTHX_ mt_xs_dec_t *d, size_t n)
{
    if ((size_t)(d->end - d->p) < n)
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
}

#define DEC_U8(d)  (S_dec_need(aTHX_ d, 1), (U8)*(d)->p++)
static U16 S_dec_u16(pTHX_ mt_xs_dec_t *d) { U16 v; S_dec_need(aTHX_ d, 2); memcpy(&v, d->p, 2); d->p += 2; return v; }
static U32 S_dec_u32(pTHX_ mt_xs_dec_t *d) { U32 v; S_dec_need(aTHX_ d, 4); memcpy(&v, d->p, 4); d->p += 4; return v; }
static U64 S_dec_u64(pTHX_ mt_xs_dec_t *d) { U64 v; S_dec_need(aTHX_ d, 8); memcpy(&v, d->p, 8); d->p += 8; return v; }

static SV *S_dec_sv(pTHX_ mt_xs_dec_t *d);

static CV *
S_tramp_new(pTHX_ U32 slot, U32 id);

/* a handle arriving: on the host a new handle object, in the compartment
 * the pinned SV it names */
static SV *
S_dec_handle(pTHX_ mt_xs_dec_t *d)
{
    const U32 comp = S_dec_u16(aTHX_ d);
    const U32 gen = S_dec_u16(aTHX_ d);
    const U64 ptr = S_dec_u64(aTHX_ d);
    const U8 reftype = DEC_U8(d);
    const U8 proxy = DEC_U8(d);
    const U32 clen = S_dec_u16(aTHX_ d);
    const char *cls;
    S_dec_need(aTHX_ d, clen);
    cls = d->p;
    d->p += clen;

    if (d->side == MT_XS_SIDE_COMP) {
        SV *sv;
        if (comp != Perl_mt_xs_my_comp())
            Perl_croak_mt(aTHX_ MT_REMOTE_HANDLE_AFFINITY);
        if (gen != Perl_mt_xs_my_gen()
            || !(sv = Perl_mt_xs_exported_sv(aTHX_ ptr)))
            Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
        return newRV_inc(sv);
    }
    else {
        const bool is_proxy = proxy || (d->flags & MT_XS_ENC_PROXY);
        const char * const objclass = is_proxy ? "MT::XS::Proxy::Obj" : cls;
        const STRLEN objclen = is_proxy ? sizeof("MT::XS::Proxy::Obj") - 1 : clen;
        /* the shape: a hash or array object is a tied hash or array whose
         * tie object is the handle (blessed into the tie class: the handle
         * record keeps the real class); anything else is the handle itself,
         * blessed into the real class, a code ref into MT::XS::RemoteCode */
        if (reftype == SVt_PVHV || reftype == SVt_PVAV) {
            const char * const tieclass = reftype == SVt_PVHV ? "MT::XS::TiedHash"
                                                              : "MT::XS::TiedArray";
            SV * const tieobj = Perl_mt_xs_handle_new(aTHX_ comp, gen, ptr, reftype,
                                                      is_proxy, cls, clen,
                                                      tieclass, strlen(tieclass));
            SV * const shape = reftype == SVt_PVHV ? (SV *)newHV() : (SV *)newAV();
            SV *obj;
            sv_magic(shape, tieobj, PERL_MAGIC_tied, NULL, 0);
            SvREFCNT_dec(tieobj);               /* the magic holds it */
            obj = newRV_noinc(shape);
            if (objclen)
                sv_bless(obj, gv_stashpvn(objclass, objclen, GV_ADD));
            return obj;
        }
        if (reftype == SVt_PVCV && !clen)
            return Perl_mt_xs_handle_new(aTHX_ comp, gen, ptr, reftype, is_proxy,
                                         cls, clen, "MT::XS::RemoteCode",
                                         sizeof("MT::XS::RemoteCode") - 1);
        return Perl_mt_xs_handle_new(aTHX_ comp, gen, ptr, reftype, is_proxy,
                                     cls, clen, objclass, objclen);
    }
}

static SV *
S_dec_sv(pTHX_ mt_xs_dec_t *d)
{
    const U8 tag = DEC_U8(d);
    switch (tag) {
    case 'u':
        return newSV_type(SVt_NULL);
    case 'b':
        return DEC_U8(d) ? newSVsv(&PL_sv_yes) : newSVsv(&PL_sv_no);
    case 'i':
        return newSViv((IV)S_dec_u64(aTHX_ d));
    case 'U':
        return newSVuv((UV)S_dec_u64(aTHX_ d));
    case 'n': {
        NV nv;
        S_dec_need(aTHX_ d, sizeof nv);
        memcpy(&nv, d->p, sizeof nv);
        d->p += sizeof nv;
        return newSVnv(nv);
    }
    case 'p': {
        const bool utf8 = cBOOL(DEC_U8(d));
        const U32 len = S_dec_u32(aTHX_ d);
        SV *sv;
        S_dec_need(aTHX_ d, len);
        sv = newSVpvn(d->p, len);
        d->p += len;
        if (utf8)
            SvUTF8_on(sv);
        return sv;
    }
    case 'P': {
        const bool utf8 = cBOOL(DEC_U8(d));
        const U32 shm = S_dec_u32(aTHX_ d);
        const U64 off = S_dec_u64(aTHX_ d);
        const U64 len = S_dec_u64(aTHX_ d);
        const char *src;
        SV *sv;
        if (shm != 0)
            Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
        src = (const char *)Perl_mt_xs_arena_at(aTHX_ d->slot, off, len);
        sv = newSVpvn(src, (STRLEN)len);
        if (utf8)
            SvUTF8_on(sv);
        return sv;
    }
    case 'd': {
        const U8 kind = DEC_U8(d);
        NV nv = 0;
        U64 u = 0;
        bool utf8;
        U32 len;
        SV *sv;
        if (kind == 'n') {
            S_dec_need(aTHX_ d, sizeof nv);
            memcpy(&nv, d->p, sizeof nv);
            d->p += sizeof nv;
        }
        else
            u = S_dec_u64(aTHX_ d);
        utf8 = cBOOL(DEC_U8(d));
        len = S_dec_u32(aTHX_ d);
        S_dec_need(aTHX_ d, len);
        sv = newSVpvn(d->p, len);
        d->p += len;
        if (utf8)
            SvUTF8_on(sv);
        sv_upgrade(sv, SVt_PVNV);
        if (kind == 'n') {
            SvNV_set(sv, nv);
            SvNOK_on(sv);
        }
        else {
            SvIV_set(sv, (IV)u);
            SvIOK_on(sv);
            if (kind == 'U')
                SvIsUV_on(sv);
        }
        return sv;
    }
    case 'R': {
        SV * const inner = S_dec_sv(aTHX_ d);
        return newRV_noinc(inner);
    }
    case 'A': {
        const U32 n = S_dec_u32(aTHX_ d);
        AV * const av = newAV();
        SV * const rv = newRV_noinc((SV *)av);
        U32 i;
        SAVEFREESV(rv);
        if (n)
            av_extend(av, n - 1);
        for (i = 0; i < n; i++)
            av_push(av, S_dec_sv(aTHX_ d));
        return SvREFCNT_inc_simple_NN(rv);
    }
    case 'H': {
        const U32 n = S_dec_u32(aTHX_ d);
        HV * const hv = newHV();
        SV * const rv = newRV_noinc((SV *)hv);
        U32 i;
        SAVEFREESV(rv);
        for (i = 0; i < n; i++) {
            const bool kutf8 = cBOOL(DEC_U8(d));
            const U32 klen = S_dec_u32(aTHX_ d);
            const char *k;
            SV *val;
            S_dec_need(aTHX_ d, klen);
            k = d->p;
            d->p += klen;
            val = S_dec_sv(aTHX_ d);
            (void)hv_common(hv, NULL, k, klen, kutf8 ? HVhek_UTF8 : 0,
                            HV_FETCH_ISSTORE, val, 0);
        }
        return SvREFCNT_inc_simple_NN(rv);
    }
    case 'B': {
        const U32 clen = S_dec_u16(aTHX_ d);
        const char *cls;
        SV *inner;
        SV *rv;
        S_dec_need(aTHX_ d, clen);
        cls = d->p;
        d->p += clen;
        inner = S_dec_sv(aTHX_ d);
        if (SvROK(inner)) {
            /* the body was an A, H or R: bless what it refers to */
            rv = inner;
        }
        else
            rv = newRV_noinc(inner);
        sv_bless(rv, gv_stashpvn(cls, clen, GV_ADD));
        return rv;
    }
    case 'C': {
        const U32 slot = S_dec_u32(aTHX_ d);
        const U32 id = S_dec_u32(aTHX_ d);
        if (d->side == MT_XS_SIDE_HOST) {
            /* our own code ref coming back */
            SV **svp = PL_mt_xs_hh ? av_fetch(PL_mt_xs_hh, id, 0) : NULL;
            if (slot != d->slot || !svp || !SvROK(*svp))
                Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
            return newSVsv(*svp);
        }
        return newRV_noinc((SV *)S_tramp_new(aTHX_ slot, id));
    }
    case 'h':
        return S_dec_handle(aTHX_ d);
    default:
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    }
}

/* ---- payloads ---------------------------------------------------------------- */

/* copy a finished stream into the arena and fill the frame's off/len */
static void
S_enc_finish(pTHX_ mt_xs_enc_t *e, mt_xs_frame_t *f)
{
    const STRLEN len = SvCUR(e->buf);
    U64 off;
    char * const dst = (char *)Perl_mt_xs_arena_alloc(aTHX_ e->slot, len ? len : 1, &off);
    Copy(SvPVX(e->buf), dst, len, char);
    f->off = off;
    f->len = len;
}

static void
S_dec_init(pTHX_ mt_xs_dec_t *d, const mt_xs_frame_t *f, int side, U32 comp)
{
    d->p = (f->flags & MT_XSF_FLAG_COPIED)
        ? (const char *)INT2PTR(void *, f->pad[0])
        : (const char *)Perl_mt_xs_arena_at(aTHX_ f->slot, f->off, f->len);
    d->end = d->p + f->len;
    d->slot = f->slot;
    d->side = side;
    d->flags = 0;
    d->comp = comp;
    d->gen = 0;
}

/* the error of a DIE frame as an SV (mortal) */
static SV *
S_dec_error(pTHX_ const mt_xs_frame_t *f, int side)
{
    mt_xs_dec_t d;
    SV *err;
    if (f->flags & MT_XSF_FLAG_PLAIN_DIE) {
        /* the plain-string form mt_xs_compartment.c uses */
        const char *p = (f->flags & MT_XSF_FLAG_COPIED)
            ? (const char *)INT2PTR(void *, f->pad[0])
            : (const char *)Perl_mt_xs_arena_at(aTHX_ f->slot, f->off, f->len);
        U32 len;
        memcpy(&len, p, 4);
        return sv_2mortal(newSVpvn(p + 4, len));
    }
    S_dec_init(aTHX_ &d, f, side, 0);
    err = sv_2mortal(S_dec_sv(aTHX_ &d));
    if (side == MT_XS_SIDE_HOST && !SvROK(err) && SvPOK(err)) {
        /* a croak raised in the compartment with no Perl frame of its own
         * carries no location, just ".\n": let the host's croak add the
         * caller's */
        STRLEN len;
        const char * const p = SvPV(err, len);
        if (len >= 2 && p[len - 1] == '\n' && p[len - 2] == '.'
            && !memmem(p, len, " line ", 6))
            SvCUR_set(err, len - 2);
    }
    return err;
}

/* ---- the compartment: trampolines (host code called from XS) ------------ */

static U32 S_cb_depth;      /* compartment: nesting of outstanding callbacks */

static int
S_tramp_free(pTHX_ SV *sv, MAGIC *mg)
{
    const mt_xs_tramp_t * const t = (const mt_xs_tramp_t *)SvPVX(sv);
    mt_xs_frame_t f;
    PERL_UNUSED_ARG(mg);
    Zero(&f, 1, mt_xs_frame_t);
    f.type = MT_XSF_HREL;
    f.slot = t->slot;
    f.a = t->id;
    Perl_mt_xs_reply(aTHX_ &f);
    return 0;
}

static const MGVTBL S_tramp_vtbl = { 0, 0, 0, 0, S_tramp_free, 0, 0, 0 };

static CV *
S_tramp_new(pTHX_ U32 slot, U32 id)
{
    CV * const cv = newXS_flags(NULL, S_tramp_xsub, "mt_xs_bridge.c", NULL, 0);
    SV * const box = newSV(sizeof(mt_xs_tramp_t));
    mt_xs_tramp_t *t;
    SvPOK_only(box);
    SvCUR_set(box, sizeof(mt_xs_tramp_t));
    t = (mt_xs_tramp_t *)SvPVX(box);
    t->slot = slot;
    t->id = id;
    sv_magicext(box, NULL, PERL_MAGIC_ext, &S_tramp_vtbl, NULL, 0);
    CvXSUBANY(cv).any_sv = box;
    CvREFCOUNTED_ANYSV_on(cv);
    return cv;
}

/* the compartment side of a host callback: marshal the arguments, post
 * CALLBACK on the owner's reply ring, wait for CB_RET or CB_DIE */
static void
S_tramp_xsub(pTHX_ CV *cv)
{
    dXSARGS;
    const mt_xs_tramp_t * const t = (const mt_xs_tramp_t *)SvPVX(CvXSUBANY(cv).any_sv);
    mt_xs_enc_t e;
    mt_xs_frame_t f, r;
    SSize_t i;
    const U32 depth = ++S_cb_depth;
    const I32 gimme = GIMME_V;

    if (!Perl_mt_xs_slot_waiting(aTHX_ t->slot)) {
        S_cb_depth--;
        Perl_croak(aTHX_ "perl5-mt: the worker that owns this callback is not "
                         "waiting on this compartment (MT_COMPARTMENT_DEAD)");
    }
    S_enc_init(aTHX_ &e, t->slot, MT_XS_SIDE_COMP, 0);
    ENC_U32(&e, items);
    for (i = 0; i < items; i++)
        S_enc_sv(aTHX_ &e, ST(i));
    Zero(&f, 1, mt_xs_frame_t);
    f.type = MT_XSF_CALLBACK;
    f.slot = t->slot;
    f.depth = depth;
    f.gimme = (U32)gimme;
    f.a = t->id;
    S_enc_finish(aTHX_ &e, &f);
    Perl_mt_xs_reply(aTHX_ &f);
    {
        /* a nested call served during the wait may grow the stack: keep the
         * position as an offset from its base, not as a pointer */
        const SSize_t sp_off = SP - PL_stack_base;
        const int got = Perl_mt_xs_comp_wait_cb(aTHX_ t->slot, depth, &r);
        SP = PL_stack_base + sp_off;
        if (!got) {
            S_cb_depth--;
            Perl_croak(aTHX_ "perl5-mt: the worker that owns this callback is not "
                             "waiting on this compartment (MT_COMPARTMENT_DEAD)");
        }
    }
    S_cb_depth--;
    SP -= items;
    if (r.type == MT_XSF_CB_DIE) {
        SV * const err = S_dec_error(aTHX_ &r, MT_XS_SIDE_COMP);
        PUTBACK;
        Perl_croak_sv(aTHX_ err);
    }
    {
        mt_xs_dec_t d;
        SSize_t n, k;
        S_dec_init(aTHX_ &d, &r, MT_XS_SIDE_COMP, 0);
        n = (SSize_t)S_dec_u32(aTHX_ &d);
        EXTEND(SP, n);
        for (k = 0; k < n; k++)
            PUSHs(sv_2mortal(S_dec_sv(aTHX_ &d)));
        if (gimme == G_SCALAR && n == 0)
            PUSHs(&PL_sv_undef);
    }
    PUTBACK;
}

/* ---- the compartment: serving frames ------------------------------------------ */

static void
S_reply_die_sv(pTHX_ const mt_xs_frame_t *req, SV *err)
{
    mt_xs_enc_t e;
    mt_xs_frame_t r;
    Zero(&r, 1, mt_xs_frame_t);
    r.type = MT_XSF_DIE;
    r.slot = req->slot;
    r.seq = req->seq;
    r.depth = req->depth;
    S_enc_init(aTHX_ &e, req->slot, MT_XS_SIDE_COMP, MT_XS_ENC_DEEP_BLESSED);
    S_enc_sv(aTHX_ &e, err);
    S_enc_finish(aTHX_ &e, &r);
    Perl_mt_xs_reply(aTHX_ &r);
}

/* magic a compartment XSUB attached to a host argument is a lost
 * semantic: refuse */
static void
S_check_host_magic(pTHX_ SV **args, SSize_t n)
{
    SSize_t i;
    for (i = 0; i < n; i++) {
        SV *sv = args[i];
        if (!sv)
            continue;
        if (SvMAGICAL(sv) && SvRMAGICAL(sv) && mg_find(sv, PERL_MAGIC_ext))
            Perl_croak_mt(aTHX_ MT_COMPARTMENT_MAGIC_ON_HOST_SV);
        if (SvTYPE(sv) >= SVt_PVMG && SvMAGIC(sv)) {
            /* any magic at all: the SV was plain when it was made */
            MAGIC *mg;
            for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
                if (mg->mg_type != PERL_MAGIC_taint
                    && mg->mg_type != PERL_MAGIC_utf8)
                    Perl_croak_mt(aTHX_ MT_COMPARTMENT_MAGIC_ON_HOST_SV);
        }
        if (SvROK(sv) && !SvOBJECT(SvRV(sv))) {
            SV * const ref = SvRV(sv);
            if (SvTYPE(ref) >= SVt_PVMG && SvMAGIC(ref)) {
                MAGIC *mg;
                for (mg = SvMAGIC(ref); mg; mg = mg->mg_moremagic)
                    if (mg->mg_type != PERL_MAGIC_taint
                        && mg->mg_type != PERL_MAGIC_utf8)
                        Perl_croak_mt(aTHX_ MT_COMPARTMENT_MAGIC_ON_HOST_SV);
            }
        }
    }
}

static bool
S_scalar_changed(pTHX_ SV *snap, SV *now)
{
    if (!snap)
        return FALSE;
    if (SvROK(now))
        return TRUE;
    if ((SvOK(snap) != SvOK(now)))
        return TRUE;
    if (!SvOK(now))
        return FALSE;
    if (SvUTF8(snap) != SvUTF8(now))
        return TRUE;
    return !sv_eq(snap, now);
}

/* the part of RET after the results: write-backs and the newXS report */
static void
S_enc_epilogue(pTHX_ mt_xs_enc_t *e, SV **args, SV **snaps, SSize_t nargs, AV *log)
{
    STRLEN mark;
    U32 n = 0;
    SSize_t i, k;

    mark = SvCUR(e->buf);
    ENC_U32(e, 0);
    for (i = 0; i < nargs; i++) {
        SV *sv = args[i];
        if (!sv || !snaps[i])
            continue;
        if (SvROK(snaps[i])) {
            /* R: the referent of a ref to a scalar */
            SV * const now = SvRV(sv);
            if (SvROK(sv) && S_scalar_changed(aTHX_ SvRV(snaps[i]), now)) {
                ENC_U32(e, (U32)i | 0x80000000u);
                S_enc_sv(aTHX_ e, now);
                n++;
            }
        }
        else if (S_scalar_changed(aTHX_ snaps[i], sv)) {
            ENC_U32(e, (U32)i);
            S_enc_sv(aTHX_ e, sv);
            n++;
        }
    }
    Copy(&n, SvPVX(e->buf) + mark, 4, char);

    /* XSUBs defined meanwhile (DBI's _install_method does this) */
    n = 0;
    mark = SvCUR(e->buf);
    ENC_U32(e, 0);
    if (log) {
        for (k = 0; k <= av_top_index(log); k++) {
            CV * const cv = (CV *)*av_fetch(log, k, 0);
            SV * const name = sv_2mortal(newSVpvs(""));
            if (!CvISXSUB(cv) || !CvGV(cv))
                continue;
            gv_fullname4(name, CvGV(cv), NULL, TRUE);
            if (!SvCUR(name))
                continue;
            Perl_mt_xs_register_xsub(aTHX_ name, cv);
            ENC_U32(e, SvCUR(name));
            ENC_RAW(e, SvPVX(name), SvCUR(name));
            ENC_U64(e, PTR2UV(cv));
            ENC_U8(e, SvPOK(cv) ? 1 : 0);
            ENC_U32(e, SvPOK(cv) ? SvCUR(cv) : 0);
            if (SvPOK(cv))
                ENC_RAW(e, SvPVX(cv), SvCUR(cv));
            n++;
        }
    }
    Copy(&n, SvPVX(e->buf) + mark, 4, char);
}

/* snapshots for the write-back comparison: plain scalars and refs to
 * plain scalars */
static SV *
S_snapshot(pTHX_ SV *sv)
{
    /* the argument is a mortal with one reference: a plain copy would
     * steal its buffer, hence NOSTEAL */
    if (!SvROK(sv))
        return sv_mortalcopy_flags(sv, SV_GMAGIC | SV_NOSTEAL);
    if (!SvOBJECT(SvRV(sv)) && SvTYPE(SvRV(sv)) < SVt_PVAV
        && !Perl_mt_xs_handle_ref(aTHX_ sv))
        return sv_2mortal(newRV_noinc(newSVsv_flags(SvRV(sv), SV_GMAGIC | SV_NOSTEAL)));
    return NULL;
}

static void
S_comp_call_frame(pTHX_ const mt_xs_frame_t *f)
{
    dSP;
    mt_xs_dec_t d;
    SV **args, **snaps;
    SV *invocant = NULL;
    SV *method = NULL;
    U32 nargs, i;
    I32 count;
    AV *log;
    SSize_t nargs_total;
    int r_errno = 0;
    const I32 gimme = f->gimme ? (I32)f->gimme : G_SCALAR;

    S_dec_init(aTHX_ &d, f, MT_XS_SIDE_COMP, 0);
    if (f->type == MT_XSF_METHOD) {
        d.flags = f->flags & MT_XSF_FLAG_PROXY ? MT_XS_ENC_PROXY : 0;
        invocant = sv_2mortal(S_dec_sv(aTHX_ &d));
        method = sv_2mortal(S_dec_sv(aTHX_ &d));
    }
    else if (f->flags & MT_XSF_FLAG_AUTOLOAD) {
        /* an XS AUTOLOAD: the sub's name goes where gv_autoload_pvn
         * would have put it, the CV's PV, with the stash in CvSTASH */
        SV * const name = sv_2mortal(S_dec_sv(aTHX_ &d));
        SV * const cvsv = Perl_mt_xs_exported_sv(aTHX_ f->a);
        if (cvsv && SvTYPE(cvsv) == SVt_PVCV && !SvPOK(cvsv)) {
            STRLEN len;
            const char * const pv = SvPV(name, len);
            const char *colon = NULL, *q;
            for (q = pv; q + 1 < pv + len; q++)
                if (q[0] == ':' && q[1] == ':')
                    colon = q;
            sv_setpvn(cvsv, colon ? colon + 2 : pv,
                      colon ? (STRLEN)(pv + len - colon - 2) : len);
            SvPOK_off(cvsv);
            if (SvUTF8(name)) SvUTF8_on(cvsv); else SvUTF8_off(cvsv);
            CvAUTOLOAD_on((CV *)cvsv);
            if (colon)
                CvSTASH_set((CV *)cvsv, gv_stashpvn(pv, (STRLEN)(colon - pv), GV_ADD));
        }
    }
    nargs = S_dec_u32(aTHX_ &d);
    nargs_total = (SSize_t)nargs + (invocant ? 1 : 0);
    Newxz(args, nargs_total + 1, SV *);
    SAVEFREEPV(args);
    Newxz(snaps, nargs_total + 1, SV *);
    SAVEFREEPV(snaps);

    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    EXTEND(SP, nargs_total);
    if (invocant) {
        args[0] = invocant;
        PUSHs(invocant);
    }
    for (i = 0; i < nargs; i++) {
        SV * const sv = sv_2mortal(S_dec_sv(aTHX_ &d));
        args[i + (invocant ? 1 : 0)] = sv;
        snaps[i + (invocant ? 1 : 0)] = S_snapshot(aTHX_ sv);
        PUSHs(sv);
    }
    PUTBACK;

    log = newAV();
    SAVEFREESV(log);
    SAVEVPTR(PL_mt_xs_newxs_log);
    PL_mt_xs_newxs_log = log;

    /* $! is what the XSUB leaves in errno: it travels with the reply */
    SETERRNO(0, 0);
    if (f->type == MT_XSF_METHOD) {
        count = call_method(SvPV_nolen(method), gimme | G_EVAL);
    }
    else {
        SV * const cvsv = Perl_mt_xs_exported_sv(aTHX_ f->a);
        if (!cvsv || SvTYPE(cvsv) != SVt_PVCV) {
            PL_mt_xs_newxs_log = NULL;
            FREETMPS;
            LEAVE;
            Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
        }
        count = call_sv(cvsv, gimme | G_EVAL);
    }
    {
        const int saved_errno = errno;
        PL_mt_xs_newxs_log = NULL;
        SPAGAIN;
        r_errno = saved_errno;
    }

    if (SvTRUE(ERRSV)) {
        SV * const err = sv_mortalcopy(ERRSV);
        SP -= count;
        PUTBACK;
        S_reply_die_sv(aTHX_ f, err);
    }
    else {
        mt_xs_enc_t e;
        mt_xs_frame_t r;
        I32 k;
        S_check_host_magic(aTHX_ args, nargs_total);
        Zero(&r, 1, mt_xs_frame_t);
        r.type = MT_XSF_RET;
        r.slot = f->slot;
        r.seq = f->seq;
        r.depth = f->depth;
        r.b = (U64)(U32)r_errno;
        S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_COMP,
                   (f->flags & MT_XSF_FLAG_PROXY) ? MT_XS_ENC_PROXY : 0);
        ENC_U32(&e, count);
        for (k = count; k > 0; k--)
            S_enc_sv(aTHX_ &e, SP[-k + 1]);
        SP -= count;
        PUTBACK;
        S_enc_epilogue(aTHX_ &e, args, snaps, nargs_total, log);
        S_enc_finish(aTHX_ &e, &r);
        Perl_mt_xs_reply(aTHX_ &r);
    }
    FREETMPS;
    LEAVE;
}

/* TIE_* on an exported hash or array (tied in the compartment itself,
 * as DBI handles are, the tie methods run there) */
static void
S_comp_tie_frame(pTHX_ const mt_xs_frame_t *f)
{
    dSP;
    mt_xs_dec_t d;
    SV * const target = Perl_mt_xs_exported_sv(aTHX_ f->a);
    SV *key = NULL, *val = NULL;
    mt_xs_enc_t e;
    mt_xs_frame_t r;
    SV *res = NULL;
    U32 nargs;

    PERL_UNUSED_VAR(sp);
    if (!target)
        Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
    ENTER;
    SAVETMPS;
    S_dec_init(aTHX_ &d, f, MT_XS_SIDE_COMP, 0);
    nargs = S_dec_u32(aTHX_ &d);
    if (nargs > 0)
        key = sv_2mortal(S_dec_sv(aTHX_ &d));
    if (nargs > 1)
        val = sv_2mortal(S_dec_sv(aTHX_ &d));

    if (SvTYPE(target) == SVt_PVHV) {
        HV * const hv = (HV *)target;
        switch (f->type) {
        case MT_XSF_TIE_FETCH: {
            HE * const he = key ? hv_fetch_ent(hv, key, 0, 0) : NULL;
            if (he) {
                res = HeVAL(he);
                SvGETMAGIC(res);
            }
            break;
        }
        case MT_XSF_TIE_STORE:
            if (key && val) {
                SV * const copy = newSVsv(val);
                HE * const he = hv_fetch_ent(hv, key, 1, 0);
                if (he) {
                    sv_setsv(HeVAL(he), copy);
                    SvSETMAGIC(HeVAL(he));
                }
                SvREFCNT_dec(copy);
            }
            break;
        case MT_XSF_TIE_EXISTS:
            res = key && hv_exists_ent(hv, key, 0) ? &PL_sv_yes : &PL_sv_no;
            break;
        case MT_XSF_TIE_DELETE:
            res = key ? hv_delete_ent(hv, key, 0, 0) : NULL;
            break;
        case MT_XSF_TIE_CLEAR:
            hv_clear(hv);
            break;
        case MT_XSF_TIE_FIRSTKEY:
            hv_iterinit(hv);
            /* FALLTHROUGH */
        case MT_XSF_TIE_NEXTKEY: {
            HE * const he = hv_iternext(hv);
            res = he ? hv_iterkeysv(he) : NULL;       /* already mortal */
            break;
        }
        case MT_XSF_TIE_FETCHSIZE:
            res = sv_2mortal(newSVuv(HvUSEDKEYS(hv)));
            break;
        default:
            Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
        }
    }
    else if (SvTYPE(target) == SVt_PVAV) {
        AV * const av = (AV *)target;
        switch (f->type) {
        case MT_XSF_TIE_FETCH: {
            SV ** const svp = key ? av_fetch(av, SvIV(key), 0) : NULL;
            if (svp) {
                res = *svp;
                SvGETMAGIC(res);
            }
            break;
        }
        case MT_XSF_TIE_STORE:
            if (key && val) {
                SV * const copy = newSVsv(val);
                if (!av_store(av, SvIV(key), copy))
                    SvREFCNT_dec(copy);
                else
                    SvSETMAGIC(copy);
            }
            break;
        case MT_XSF_TIE_FETCHSIZE:
            res = sv_2mortal(newSViv(av_count(av)));
            break;
        case MT_XSF_TIE_PUSH:
            if (key)
                av_push(av, newSVsv(key));
            break;
        case MT_XSF_TIE_CLEAR:
            av_clear(av);
            break;
        case MT_XSF_TIE_EXISTS:
            res = key && av_exists(av, SvIV(key)) ? &PL_sv_yes : &PL_sv_no;
            break;
        case MT_XSF_TIE_DELETE:
            res = key ? av_delete(av, SvIV(key), 0) : NULL;
            break;
        default:
            Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
        }
    }
    else
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);

    Zero(&r, 1, mt_xs_frame_t);
    r.type = MT_XSF_RET;
    r.slot = f->slot;
    r.seq = f->seq;
    r.depth = f->depth;
    S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_COMP,
               (f->flags & MT_XSF_FLAG_PROXY) ? MT_XS_ENC_PROXY : 0);
    ENC_U32(&e, 1);
    S_enc_sv(aTHX_ &e, res ? res : &PL_sv_undef);
    ENC_U32(&e, 0);
    ENC_U32(&e, 0);
    S_enc_finish(aTHX_ &e, &r);
    Perl_mt_xs_reply(aTHX_ &r);
    FREETMPS;
    LEAVE;
}

/* SETVAR / GETVAR: a package variable of the compartment */
static void
S_comp_var_frame(pTHX_ const mt_xs_frame_t *f)
{
    mt_xs_dec_t d;
    SV *name, *val = NULL;
    const char *n;
    STRLEN nlen;
    char sigil;
    mt_xs_enc_t e;
    mt_xs_frame_t r;

    ENTER;
    SAVETMPS;
    S_dec_init(aTHX_ &d, f, MT_XS_SIDE_COMP, 0);
    if (S_dec_u32(aTHX_ &d) < (f->type == MT_XSF_SETVAR ? 2u : 1u))
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    name = sv_2mortal(S_dec_sv(aTHX_ &d));
    if (f->type == MT_XSF_SETVAR)
        val = sv_2mortal(S_dec_sv(aTHX_ &d));
    n = SvPV(name, nlen);
    sigil = nlen && (*n == '$' || *n == '@' || *n == '%') ? *n++ : '$';
    if (sigil != '$')
        nlen--;

    Zero(&r, 1, mt_xs_frame_t);
    r.type = MT_XSF_RET;
    r.slot = f->slot;
    r.seq = f->seq;
    r.depth = f->depth;
    S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_COMP, 0);
    ENC_U32(&e, 1);
    {
        SV * const pvname = sv_2mortal(newSVpvn(n, nlen));
        if (sigil == '$') {
            SV * const sv = get_sv(SvPVX(pvname), GV_ADD);
            if (val)
                sv_setsv_mg(sv, val);
            S_enc_sv(aTHX_ &e, sv);
        }
        else if (sigil == '@') {
            AV * const av = get_av(SvPVX(pvname), GV_ADD);
            if (val) {
                av_clear(av);
                if (SvROK(val) && SvTYPE(SvRV(val)) == SVt_PVAV) {
                    AV * const src = (AV *)SvRV(val);
                    SSize_t i;
                    for (i = 0; i < (SSize_t)av_count(src); i++) {
                        SV ** const svp = av_fetch(src, i, 0);
                        av_push(av, svp ? newSVsv(*svp) : newSV_type(SVt_NULL));
                    }
                }
            }
            S_enc_sv(aTHX_ &e, sv_2mortal(newRV_inc((SV *)av)));
        }
        else {
            HV * const hv = get_hv(SvPVX(pvname), GV_ADD);
            if (val) {
                hv_clear(hv);
                if (SvROK(val) && SvTYPE(SvRV(val)) == SVt_PVHV) {
                    HV * const src = (HV *)SvRV(val);
                    HE *he;
                    hv_iterinit(src);
                    while ((he = hv_iternext(src)))
                        (void)hv_store_ent(hv, hv_iterkeysv(he),
                                           newSVsv(hv_iterval(src, he)), 0);
                }
            }
            S_enc_sv(aTHX_ &e, sv_2mortal(newRV_inc((SV *)hv)));
        }
    }
    ENC_U32(&e, 0);
    ENC_U32(&e, 0);
    S_enc_finish(aTHX_ &e, &r);
    Perl_mt_xs_reply(aTHX_ &r);
    FREETMPS;
    LEAVE;
}

/*
=for apidoc_section $embedding

=for apidoc mt_xs_comp_serve

Inside a compartment: serves a C<CALL>, C<METHOD>, C<TIE_*>, C<SETVAR> or
C<GETVAR> frame and replies to it (C<RET>, or C<DIE> carrying the error
when the call died; a croak from the service itself is caught by the
caller in mt_xs_compartment.c and answered the same way).

=cut
*/

void
Perl_mt_xs_comp_serve(pTHX_ const mt_xs_frame_t *f)
{
    PERL_ARGS_ASSERT_MT_XS_COMP_SERVE;
    switch (f->type) {
    case MT_XSF_CALL:
    case MT_XSF_METHOD:
        S_comp_call_frame(aTHX_ f);
        break;
    case MT_XSF_TIE_FETCH: case MT_XSF_TIE_STORE: case MT_XSF_TIE_EXISTS:
    case MT_XSF_TIE_DELETE: case MT_XSF_TIE_CLEAR: case MT_XSF_TIE_FIRSTKEY:
    case MT_XSF_TIE_NEXTKEY: case MT_XSF_TIE_FETCHSIZE: case MT_XSF_TIE_PUSH:
        S_comp_tie_frame(aTHX_ f);
        break;
    case MT_XSF_SETVAR:
    case MT_XSF_GETVAR:
        S_comp_var_frame(aTHX_ f);
        break;
    default:
        Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
    }
}

/* ---- the host: calls --------------------------------------------------------- */

static U64 S_remote_calls;      /* process-wide counters for MT::XS::stats */
static U64 S_callbacks;

/* Before a new call's payload goes into the slot's arena.  When no call is
 * in flight the arena is rewound.  When it is more than half full, new calls
 * of this worker are held back (a fiber passes) until the calls in flight
 * have finished, and then it is rewound: with fibers the calls may never all
 * finish at once, so without the hold the arena would fill for good.  A
 * nested call (thread mode, inside a callback) cannot wait, since the call
 * it nests in still needs its payload: it allocates, or dies at the limit. */
static void
S_arena_begin(pTHX_ U32 slot)
{
    for (;;) {
        if (Perl_mt_xs_in_call_depth(aTHX_ slot) == 0) {
            Perl_mt_xs_arena_reset(aTHX_ slot);
            PL_mt_xs_gated = 0;
            return;
        }
        if (!PL_mt_xs_gated && !Perl_mt_xs_arena_full(aTHX_ slot))
            return;
        if (!MT_FIBER_PARKABLE(PL_mt_fiber_current, PL_mt_sched_fiber))
            return;
        PL_mt_xs_gated = 1;
        Perl_mt_sched_pass(aTHX);
    }
}

/* post a frame and wait for its reply, keeping the slot's call depth

 * right whatever happens.  A scheduled fiber parks in the wait; its calls
 * are counted against PERL_MT_XS_INFLIGHT so that the reply ring cannot
 * overflow. */
static void
S_host_round_trip(pTHX_ U32 comp, U32 slot, mt_xs_frame_t *f, mt_xs_frame_t *r)
{
    dJMPENV;
    int rc;
    volatile int fiber = MT_FIBER_PARKABLE(PL_mt_fiber_current, PL_mt_sched_fiber)
                         ? 1 : 0;
    if (fiber) {
        if (PL_mt_xs_inflight >= Perl_mt_xs_inflight_max(aTHX))
            Perl_croak_mt(aTHX_ MT_XS_TOO_MANY_INFLIGHT);
        PL_mt_xs_inflight++;
    }
    Perl_mt_xs_in_call_enter(aTHX_ slot);
    Perl_mt_xs_post(aTHX_ comp, f);
    JMPENV_PUSH(rc);
    if (rc == 0)
        Perl_mt_xs_wait(aTHX_ comp, slot, f->seq, r);
    JMPENV_POP;
    if (rc == 0 && (r->flags & MT_XSF_FLAG_COPIED))
        SAVEFREEPV(INT2PTR(void *, r->pad[0]));   /* freed with the scope */
    Perl_mt_xs_in_call_leave(aTHX_ slot);
    if (fiber)
        PL_mt_xs_inflight--;
    if (rc != 0)
        JMPENV_JUMP(rc);
}

/* the stub's compartment, or a croak when it is gone */
static U32
S_stub_comp(pTHX_ const mt_xs_stub_t *st)
{
    if (!Perl_mt_xs_comp_alive(aTHX_ st->comp, st->gen)) {
        if (Perl_mt_xs_comp_alive(aTHX_ st->comp, Perl_mt_xs_comp_gen(aTHX_ st->comp)))
            Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    }
    return st->comp;
}

/* decode a RET: the results onto the stack (replacing the arguments at
 * ST(0..)), write-backs into the original argument SVs, new stubs */
static I32
S_host_results(pTHX_ const mt_xs_frame_t *r, U32 comp, SV **origargs,
               SSize_t nargs, U32 proxy)
{
    dSP;
    mt_xs_dec_t d;
    U32 n, k;
    SSize_t nres;
    S_dec_init(aTHX_ &d, r, MT_XS_SIDE_HOST, comp);
    d.flags = proxy ? MT_XS_ENC_PROXY : 0;
    nres = (SSize_t)S_dec_u32(aTHX_ &d);
    EXTEND(SP, nres);
    for (k = 0; k < (U32)nres; k++)
        PUSHs(sv_2mortal(S_dec_sv(aTHX_ &d)));
    PUTBACK;

    /* write-backs */
    n = S_dec_u32(aTHX_ &d);
    for (k = 0; k < n; k++) {
        const U32 raw = S_dec_u32(aTHX_ &d);
        const U32 idx = raw & 0x7fffffffu;
        SV * const val = sv_2mortal(S_dec_sv(aTHX_ &d));
        SV *target;
        if ((SSize_t)idx >= nargs || !origargs[idx])
            continue;
        target = origargs[idx];
        if (raw & 0x80000000u) {
            if (!SvROK(target))
                continue;
            target = SvRV(target);
        }
        if (SvREADONLY(target) || (SvFLAGS(target) & SVf_IMMORTAL))
            continue;
        sv_setsv_mg(target, val);
    }

    /* XSUBs the compartment defined during the call */
    n = S_dec_u32(aTHX_ &d);
    if (n && !proxy) {
        const U32 gen = Perl_mt_xs_comp_gen(aTHX_ comp);
        for (k = 0; k < n; k++) {
            const U32 len = S_dec_u32(aTHX_ &d);
            const char *name;
            U64 ptr;
            S_dec_need(aTHX_ &d, len);
            name = d.p;
            d.p += len;
            ptr = S_dec_u64(aTHX_ &d);
            {
                const U8 has_proto = DEC_U8(&d);
                const U32 plen = S_dec_u32(aTHX_ &d);
                const char *proto;
                S_dec_need(aTHX_ &d, plen);
                proto = d.p;
                d.p += plen;
                if (ptr)
                    Perl_mt_xs_install_stub(aTHX_ name, len, comp, gen, ptr, TRUE,
                                            has_proto ? proto : NULL, plen);
            }
        }
    }
    return 0;
}

/*
=for apidoc mt_xs_remote_call

The call of a host stub for an XSUB that lives in an XS compartment
(C<CVf_MT_REMOTE>), reached from C<Perl_rpp_invoke_xs> with the arguments
on the stack as for any XSUB.  The arguments are marshalled into the
worker's arena, a C<CALL> frame is posted to the stub's compartment and
the worker blocks on its reply ring (serving the compartment's callbacks
meanwhile) until C<RET> or C<DIE> arrives: the results replace the
arguments on the stack (scalars the XSUB modified in place are written
back to the caller's variables) or the error is re-raised here with
C<croak_sv>.  A stub whose compartment has gone croaks
C<MT_COMPARTMENT_DEAD>; one from an earlier generation of a restarted
compartment croaks C<MT_STALE_REMOTE_HANDLE>.

Fiber hook: the wait is where a fiber yields with the completion
key; see L</mt_xs_wait>.

=cut
*/

void
Perl_mt_xs_remote_call(pTHX_ CV *cv)
{
    dXSARGS;
    const mt_xs_stub_t * const st = (const mt_xs_stub_t *)SvPVX(CvXSUBANY(cv).any_sv);
    U32 comp, slot;
    mt_xs_enc_t e;
    mt_xs_frame_t f, r;
    SV **origargs;
    SSize_t i;
    const I32 gimme = GIMME_V;

    PERL_ARGS_ASSERT_MT_XS_REMOTE_CALL;

    if (st->remote_cv == 0) {
        /* Pkg::bootstrap of a module the compartment has already booted:
         * nothing to do but drop the arguments */
        XSRETURN_EMPTY;
    }
    comp = S_stub_comp(aTHX_ st);
    slot = Perl_mt_xs_slot(aTHX);
    S_arena_begin(aTHX_ slot);

    Newx(origargs, items + 1, SV *);
    SAVEFREEPV(origargs);
    for (i = 0; i < items; i++)
        origargs[i] = ST(i);

    S_enc_init(aTHX_ &e, slot, MT_XS_SIDE_HOST, 0);
    Zero(&f, 1, mt_xs_frame_t);
    if (CvAUTOLOAD(cv)) {
        /* gv_autoload_pvn left the name of the sub being autoloaded in
         * this stub's PV (the XS AUTOLOAD convention): the compartment's
         * AUTOLOAD reads it from its own CV, so it travels first */
        SV * const name = sv_2mortal(newSVpvn(SvPVX(cv), SvCUR(cv)));
        if (SvUTF8(cv))
            SvUTF8_on(name);
        S_enc_sv(aTHX_ &e, name);
        f.flags |= MT_XSF_FLAG_AUTOLOAD;
    }
    ENC_U32(&e, items);
    for (i = 0; i < items; i++)
        S_enc_sv(aTHX_ &e, ST(i));
    f.type = MT_XSF_CALL;
    f.slot = slot;
    f.seq = Perl_mt_xs_next_seq(aTHX_ slot);
    f.gimme = (U32)gimme;
    f.depth = Perl_mt_xs_in_call_depth(aTHX_ slot);
    f.a = st->remote_cv;
    S_enc_finish(aTHX_ &e, &f);
    S_remote_calls++;

    {
        const int saved_errno = errno;
        S_host_round_trip(aTHX_ comp, slot, &f, &r);
        /* $! as the XSUB left it over there (the compartment clears errno
         * before the call, as code that tests $! after an XSUB expects it
         * to have set it); our own syscalls' errno is no business of the
         * caller */
        SETERRNO(r.type == MT_XSF_RET ? (int)r.b : saved_errno, 0);
    }

    SP -= items;
    PUTBACK;
    if (r.type == MT_XSF_DIE) {
        SV * const err = S_dec_error(aTHX_ &r, MT_XS_SIDE_HOST);
        Perl_croak_sv(aTHX_ err);
    }
    S_host_results(aTHX_ &r, comp, origargs, items, 0);
    if (gimme == G_SCALAR) {
        /* exactly one value in scalar context */
        SPAGAIN;
        if (SP - PL_stack_base < ax)
            XPUSHs(&PL_sv_undef);
        else
            PL_stack_sp = PL_stack_base + ax;
    }
}

/*
=for apidoc mt_xs_host_callback

On the host, while waiting for a reply: runs the host code ref a
C<CALLBACK> frame names (with the frame's arguments, in its context) and
answers C<CB_RET> with the results, or C<CB_DIE> with the error.  The code
may itself call remote stubs: those nest.

=for apidoc mt_xs_host_hrel

On the host: releases the host code handle an C<HREL> frame names (the
compartment dropped its last reference to the trampoline).

=cut
*/

void
Perl_mt_xs_host_callback(pTHX_ U32 comp, const mt_xs_frame_t *f)
{
    dSP;
    SV **svp = PL_mt_xs_hh ? av_fetch(PL_mt_xs_hh, (SSize_t)f->a, 0) : NULL;
    mt_xs_dec_t d;
    SSize_t n, k;
    I32 count;
    mt_xs_frame_t reply;
    mt_xs_enc_t e;
    const I32 gimme = f->gimme ? (I32)f->gimme : G_SCALAR;

    PERL_ARGS_ASSERT_MT_XS_HOST_CALLBACK;
    S_callbacks++;
    Zero(&reply, 1, mt_xs_frame_t);
    reply.slot = f->slot;
    reply.depth = f->depth;
    reply.seq = f->seq;

    ENTER;
    SAVETMPS;
    if (!svp || !SvROK(*svp)) {
        SV * const err = sv_2mortal(newSVpvs("perl5-mt: callback into a released "
                                             "host code ref (MT_STALE_REMOTE_HANDLE)"));
        reply.type = MT_XSF_CB_DIE;
        S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_HOST, 0);
        S_enc_sv(aTHX_ &e, err);
        S_enc_finish(aTHX_ &e, &reply);
        Perl_mt_xs_post(aTHX_ comp, &reply);
        FREETMPS;
        LEAVE;
        return;
    }
    S_dec_init(aTHX_ &d, f, MT_XS_SIDE_HOST, comp);
    n = (SSize_t)S_dec_u32(aTHX_ &d);
    PUSHMARK(SP);
    EXTEND(SP, n);
    for (k = 0; k < n; k++)
        PUSHs(sv_2mortal(S_dec_sv(aTHX_ &d)));
    PUTBACK;
    count = call_sv(*svp, gimme | G_EVAL);
    SPAGAIN;
    if (SvTRUE(ERRSV)) {
        SV * const err = sv_mortalcopy(ERRSV);
        SP -= count;
        PUTBACK;
        reply.type = MT_XSF_CB_DIE;
        S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_HOST, 0);
        S_enc_sv(aTHX_ &e, err);
    }
    else {
        I32 j;
        reply.type = MT_XSF_CB_RET;
        S_enc_init(aTHX_ &e, f->slot, MT_XS_SIDE_HOST, 0);
        ENC_U32(&e, count);
        for (j = count; j > 0; j--)
            S_enc_sv(aTHX_ &e, SP[-j + 1]);
        SP -= count;
        PUTBACK;
    }
    S_enc_finish(aTHX_ &e, &reply);
    Perl_mt_xs_post(aTHX_ comp, &reply);
    FREETMPS;
    LEAVE;
}

void
Perl_mt_xs_host_hrel(pTHX_ const mt_xs_frame_t *f)
{
    PERL_ARGS_ASSERT_MT_XS_HOST_HREL;
    if (PL_mt_xs_hh && (SSize_t)f->a <= av_top_index(PL_mt_xs_hh)) {
        SV ** const svp = av_fetch(PL_mt_xs_hh, (SSize_t)f->a, 0);
        if (svp)
            sv_setsv(*svp, &PL_sv_undef);
    }
}

/*
=for apidoc mt_xs_host_handles_free

Drops the host code handles of this interpreter (at worker exit, before
its SVs go).

=cut
*/

void
Perl_mt_xs_host_handles_free(pTHX)
{
    SvREFCNT_dec(PL_mt_xs_hh);
    PL_mt_xs_hh = NULL;
}

/* ---- the host: generic frames (TIE_*, METHOD, SETVAR, GETVAR) ------------ */

/* one request carrying `n` values; returns the single result (mortal) */
static SV *
S_host_simple_frame(pTHX_ U32 comp, U32 type, U64 a, U32 flags, SV **vals,
                    U32 n, SV *invocant, SV *method, I32 gimme, SSize_t *nres)
{
    const U32 slot = Perl_mt_xs_slot(aTHX);
    mt_xs_enc_t e;
    mt_xs_frame_t f, r;
    U32 i;
    SV *res = NULL;

    if (!Perl_mt_xs_comp_alive(aTHX_ comp, Perl_mt_xs_comp_gen(aTHX_ comp)))
        Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
    S_arena_begin(aTHX_ slot);
    S_enc_init(aTHX_ &e, slot, MT_XS_SIDE_HOST, 0);
    if (type == MT_XSF_METHOD) {
        S_enc_sv(aTHX_ &e, invocant);
        S_enc_sv(aTHX_ &e, method);
    }
    ENC_U32(&e, n);
    for (i = 0; i < n; i++)
        S_enc_sv(aTHX_ &e, vals[i]);
    Zero(&f, 1, mt_xs_frame_t);
    f.type = type;
    f.slot = slot;
    f.seq = Perl_mt_xs_next_seq(aTHX_ slot);
    f.gimme = (U32)gimme;
    f.flags = flags;
    f.depth = Perl_mt_xs_in_call_depth(aTHX_ slot);
    f.a = a;
    S_enc_finish(aTHX_ &e, &f);
    S_remote_calls++;
    {
        const int saved_errno = errno;
        S_host_round_trip(aTHX_ comp, slot, &f, &r);
        SETERRNO(r.type == MT_XSF_RET ? (int)r.b : saved_errno, 0);
    }
    if (r.type == MT_XSF_DIE)
        Perl_croak_sv(aTHX_ S_dec_error(aTHX_ &r, MT_XS_SIDE_HOST));
    {
        dSP;
        SV ** const base = PL_stack_sp;
        SSize_t got;
        PUTBACK;
        S_host_results(aTHX_ &r, comp, NULL, 0, flags & MT_XSF_FLAG_PROXY);
        SPAGAIN;
        got = SP - base;
        if (nres) {
            *nres = got;
            return NULL;                          /* the caller takes them */
        }
        res = got > 0 ? base[1] : &PL_sv_undef;
        SP = base;
        PUTBACK;
    }
    return res;
}

/* the handle record behind a handle, a tie object, or a hash or array
 * shape (whose tie object is the handle) */
static perl_mt_cap_ref_t *
S_handle_of(pTHX_ SV *sv)
{
    perl_mt_cap_ref_t *ref = Perl_mt_xs_handle_ref(aTHX_ sv);
    if (!ref && SvROK(sv))
        ref = S_tied_shape_ref(aTHX_ SvRV(sv));
    return ref;
}

/* the handle behind a tie object or proxy object, with its compartment */
static perl_mt_cap_ref_t *
S_live_handle(pTHX_ SV *sv, U32 *comp)
{
    perl_mt_cap_ref_t * const ref = S_handle_of(aTHX_ sv);
    if (!ref)
        Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
    if (!ref->ptr || !Perl_mt_xs_comp_alive(aTHX_ ref->comp, ref->cgen)) {
        if (ref->ptr && !Perl_mt_xs_comp_alive(aTHX_ ref->comp,
                                               Perl_mt_xs_comp_gen(aTHX_ ref->comp)))
            Perl_croak_mt(aTHX_ MT_COMPARTMENT_DEAD);
        Perl_croak_mt(aTHX_ MT_STALE_REMOTE_HANDLE);
    }
    *comp = ref->comp;
    return ref;
}

static void
S_tie_xs(pTHX_ CV *cv, U32 type, int minargs, const char *usage)
{
    dXSARGS;
    U32 comp;
    perl_mt_cap_ref_t *ref;
    SV *res;
    if (items < minargs)
        croak_xs_usage(cv, usage);
    ref = S_live_handle(aTHX_ ST(0), &comp);
    res = S_host_simple_frame(aTHX_ comp, type, ref->ptr,
                              ref->proxy ? MT_XSF_FLAG_PROXY : 0,
                              items > 1 ? &ST(1) : NULL, (U32)(items - 1),
                              NULL, NULL, G_SCALAR, NULL);
    SP = PL_stack_base + ax - 1;
    XPUSHs(res ? res : &PL_sv_undef);
    PUTBACK;
}

#define TIE_XS(name, type, min, usage)                      \
    XS(name);                                               \
    XS(name) { S_tie_xs(aTHX_ cv, type, min, usage); }

TIE_XS(XS_MT_XS_TiedHash_FETCH,     MT_XSF_TIE_FETCH,     2, "self, key")
TIE_XS(XS_MT_XS_TiedHash_STORE,     MT_XSF_TIE_STORE,     3, "self, key, value")
TIE_XS(XS_MT_XS_TiedHash_EXISTS,    MT_XSF_TIE_EXISTS,    2, "self, key")
TIE_XS(XS_MT_XS_TiedHash_DELETE,    MT_XSF_TIE_DELETE,    2, "self, key")
TIE_XS(XS_MT_XS_TiedHash_CLEAR,     MT_XSF_TIE_CLEAR,     1, "self")
TIE_XS(XS_MT_XS_TiedHash_FIRSTKEY,  MT_XSF_TIE_FIRSTKEY,  1, "self")
TIE_XS(XS_MT_XS_TiedHash_NEXTKEY,   MT_XSF_TIE_NEXTKEY,   1, "self, lastkey")
TIE_XS(XS_MT_XS_TiedHash_SCALAR,    MT_XSF_TIE_FETCHSIZE, 1, "self")
TIE_XS(XS_MT_XS_TiedArray_FETCH,    MT_XSF_TIE_FETCH,     2, "self, index")
TIE_XS(XS_MT_XS_TiedArray_STORE,    MT_XSF_TIE_STORE,     3, "self, index, value")
TIE_XS(XS_MT_XS_TiedArray_FETCHSIZE, MT_XSF_TIE_FETCHSIZE, 1, "self")
TIE_XS(XS_MT_XS_TiedArray_PUSH,     MT_XSF_TIE_PUSH,      2, "self, value")
TIE_XS(XS_MT_XS_TiedArray_CLEAR,    MT_XSF_TIE_CLEAR,     1, "self")
TIE_XS(XS_MT_XS_TiedArray_EXISTS,   MT_XSF_TIE_EXISTS,    2, "self, index")
TIE_XS(XS_MT_XS_TiedArray_DELETE,   MT_XSF_TIE_DELETE,    2, "self, index")

XS(XS_MT_XS_Tied_unsupported);
XS(XS_MT_XS_Tied_unsupported)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    Perl_croak_mt(aTHX_ MT_REMOTE_UNSUPPORTED_TYPE);
}

XS(XS_MT_XS_noop);
XS(XS_MT_XS_noop)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    XSRETURN_EMPTY;
}

/* ---- the host: MT::XS::Proxy and friends ------------------------------------- */

/* the method name AUTOLOAD was called for, from $Pkg::AUTOLOAD */
static SV *
S_autoload_method(pTHX_ const char *pkg)
{
    SV * const full = get_sv(Perl_form(aTHX_ "%s::AUTOLOAD", pkg), 0);
    const char *p;
    STRLEN len;
    if (!full || !SvOK(full))
        Perl_croak(aTHX_ "perl5-mt: AUTOLOAD without a method name (MT_NO_COMPARTMENT)");
    p = SvPV(full, len);
    {
        const char *last = p + len;
        while (last > p && !(last[-1] == ':' && last > p + 1 && last[-2] == ':'))
            last--;
        return sv_2mortal(newSVpvn(last, (STRLEN)(p + len - last)));
    }
}

/* a method call through the bridge: results onto the stack; ax and items
 * are the caller's (its dXSARGS popped the mark) */
static void
S_proxy_method(pTHX_ Stack_off_t ax, SSize_t items, U32 comp, SV *invocant,
               SV *method, U32 flags)
{
    dSP;
    SSize_t nres = 0;
    const I32 gimme = GIMME_V;
    SV **base;
    /* arguments are ST(1..): the invocant is sent separately */
    base = PL_stack_base + ax - 1;
    {
        SV **vals;
        SSize_t i;
        Newx(vals, items, SV *);
        SAVEFREEPV(vals);
        for (i = 1; i < items; i++)
            vals[i - 1] = ST(i);
        SP = base;
        PUTBACK;
        (void)S_host_simple_frame(aTHX_ comp, MT_XSF_METHOD, 0, flags, vals,
                                  (U32)(items - 1), invocant, method, gimme, &nres);
    }
    /* the results are now on the stack above base; scalar context keeps
     * one */
    SPAGAIN;
    if (gimme == G_SCALAR) {
        if (nres == 0)
            XPUSHs(&PL_sv_undef);
        else {
            SV * const one = SP[-nres + 1];
            SP = base;
            XPUSHs(one);
        }
    }
    PUTBACK;
}

XS(XS_MT_XS_remote);
XS(XS_MT_XS_remote)
{
    dXSARGS;
    SV *module;
    U32 comp;
    HV *self;
    if (items < 1 || items > 2)
        croak_xs_usage(cv, "module, [compartment]");
    module = ST(0);
    if (items == 2) {
        comp = (U32)SvUV(ST(1));
        if (comp >= Perl_mt_xs_comp_count(aTHX))
            Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    }
    else
        comp = (U32)Perl_mt_xs_bind_comp(aTHX);
    (void)Perl_mt_xs_remote_load_in(aTHX_ comp, module, NULL, MT_XSF_FLAG_PROXY);
    self = newHV();
    (void)hv_stores(self, "class", newSVsv(module));
    (void)hv_stores(self, "compartment", newSVuv(comp));
    ST(0) = sv_2mortal(sv_bless(newRV_noinc((SV *)self), gv_stashpvs("MT::XS::Proxy", GV_ADD)));
    XSRETURN(1);
}

XS(XS_MT_XS_Proxy_AUTOLOAD);
XS(XS_MT_XS_Proxy_AUTOLOAD)
{
    dXSARGS;
    SV *self, **svp;
    HV *hv;
    SV *method, *cls;
    U32 comp;
    if (items < 1 || !SvROK(ST(0)) || SvTYPE(SvRV(ST(0))) != SVt_PVHV)
        croak_xs_usage(cv, "proxy, args...");
    self = ST(0);
    hv = (HV *)SvRV(self);
    method = S_autoload_method(aTHX_ "MT::XS::Proxy");
    svp = hv_fetchs(hv, "class", 0);
    cls = svp ? *svp : &PL_sv_undef;
    svp = hv_fetchs(hv, "compartment", 0);
    comp = svp ? (U32)SvUV(*svp) : 0;
    S_proxy_method(aTHX_ ax, items, comp, cls, method, MT_XSF_FLAG_PROXY);
}

XS(XS_MT_XS_Proxy_Obj_AUTOLOAD);
XS(XS_MT_XS_Proxy_Obj_AUTOLOAD)
{
    dXSARGS;
    U32 comp;
    SV *method;
    if (items < 1)
        croak_xs_usage(cv, "object, args...");
    (void)S_live_handle(aTHX_ ST(0), &comp);
    method = S_autoload_method(aTHX_ "MT::XS::Proxy::Obj");
    S_proxy_method(aTHX_ ax, items, comp, ST(0), method, MT_XSF_FLAG_PROXY);
}

XS(XS_MT_XS_Proxy_Obj_MT_class);
XS(XS_MT_XS_Proxy_Obj_MT_class)
{
    dXSARGS;
    perl_mt_cap_ref_t *ref;
    if (items != 1)
        croak_xs_usage(cv, "object");
    ref = S_handle_of(aTHX_ ST(0));
    ST(0) = ref && ref->rclass ? sv_2mortal(newSVpv(ref->rclass, 0)) : &PL_sv_undef;
    XSRETURN(1);
}

XS(XS_MT_XS_RemoteCode_call);
XS(XS_MT_XS_RemoteCode_call)
{
    dXSARGS;
    U32 comp;
    perl_mt_cap_ref_t *ref;
    SSize_t nres = 0;
    const I32 gimme = GIMME_V;
    SV **base = PL_stack_base + ax - 1;
    if (items < 1)
        croak_xs_usage(cv, "code, args...");
    ref = S_live_handle(aTHX_ ST(0), &comp);
    {
        SV **vals;
        SSize_t i;
        Newx(vals, items, SV *);
        SAVEFREEPV(vals);
        for (i = 1; i < items; i++)
            vals[i - 1] = ST(i);
        SP = base;
        PUTBACK;
        (void)S_host_simple_frame(aTHX_ comp, MT_XSF_CALL, ref->ptr, 0, vals,
                                  (U32)(items - 1), NULL, NULL, gimme, &nres);
    }
    SPAGAIN;
    if (gimme == G_SCALAR) {
        if (nres == 0)
            XPUSHs(&PL_sv_undef);
        else {
            SV * const one = SP[-nres + 1];
            SP = base;
            XPUSHs(one);
        }
    }
    PUTBACK;
}

XS(XS_MT_XS_set_remote_var);
XS(XS_MT_XS_set_remote_var)
{
    dXSARGS;
    U32 comp;
    SV *vals[2];
    if (items < 2 || items > 3)
        croak_xs_usage(cv, "name, value, [compartment]");
    comp = items == 3 ? (U32)SvUV(ST(2)) : (U32)Perl_mt_xs_bind_comp(aTHX);
    if (comp >= Perl_mt_xs_comp_count(aTHX))
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    vals[0] = ST(0);
    vals[1] = ST(1);
    ST(0) = S_host_simple_frame(aTHX_ comp, MT_XSF_SETVAR, 0, 0, vals, 2,
                                NULL, NULL, G_SCALAR, NULL);
    XSRETURN(1);
}

XS(XS_MT_XS_get_remote_var);
XS(XS_MT_XS_get_remote_var)
{
    dXSARGS;
    U32 comp;
    SV *vals[1];
    if (items < 1 || items > 2)
        croak_xs_usage(cv, "name, [compartment]");
    comp = items == 2 ? (U32)SvUV(ST(1)) : (U32)Perl_mt_xs_bind_comp(aTHX);
    if (comp >= Perl_mt_xs_comp_count(aTHX))
        Perl_croak_mt(aTHX_ MT_NO_COMPARTMENT);
    vals[0] = ST(0);
    ST(0) = S_host_simple_frame(aTHX_ comp, MT_XSF_GETVAR, 0, 0, vals, 1,
                                NULL, NULL, G_SCALAR, NULL);
    XSRETURN(1);
}

XS(XS_MT_XS_is_handle);
XS(XS_MT_XS_is_handle)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "sv");
    ST(0) = S_handle_of(aTHX_ ST(0)) ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

XS(XS_MT_XS_handle_compartment);
XS(XS_MT_XS_handle_compartment)
{
    dXSARGS;
    perl_mt_cap_ref_t *ref;
    if (items != 1)
        croak_xs_usage(cv, "sv");
    ref = S_handle_of(aTHX_ ST(0));
    ST(0) = ref ? sv_2mortal(newSViv(ref->comp)) : &PL_sv_undef;
    XSRETURN(1);
}

XS(XS_MT_XS_bridge_stats);
XS(XS_MT_XS_bridge_stats)
{
    dXSARGS;
    HV * const hv = newHV();
    PERL_UNUSED_VAR(items);
    (void)hv_stores(hv, "remote_calls", newSVuv(S_remote_calls));
    (void)hv_stores(hv, "callbacks", newSVuv(S_callbacks));
    (void)hv_stores(hv, "host_handles",
                    newSViv(PL_mt_xs_hh ? av_count(PL_mt_xs_hh) : 0));
    /* Wait for compartment reply */
    (void)hv_stores(hv, "wait_tier", newSVpv(perl_krt_wait_tier(), 0));
    (void)hv_stores(hv, "poll_budget_ns", newSVuv(PL_mt_xs_poll.cur_ns));
    (void)hv_stores(hv, "polled", newSVuv(PL_mt_xs_poll.hits));
    (void)hv_stores(hv, "parked", newSVuv(PL_mt_xs_poll.misses));
    (void)hv_stores(hv, "inflight", newSVuv(PL_mt_xs_inflight));
    (void)hv_stores(hv, "inflight_max", newSVuv(Perl_mt_xs_inflight_max(aTHX)));
    (void)hv_stores(hv, "pinned_cpu", newSViv(PL_mt_xs_pinned >= 0 ? PL_mt_xs_pinned : -1));
    (void)hv_stores(hv, "affinity", newSVpv(Perl_mt_xs_affinity(aTHX) ? "smt" : "off", 0));
    ST(0) = sv_2mortal(newRV_noinc((SV *)hv));
    XSRETURN(1);
}

/*
=for apidoc mt_xs_boot_bridge

Installs the Perl-visible side of the bridge: C<MT::XS::remote>,
C<MT::XS::set_remote_var>, C<MT::XS::get_remote_var>, the tied shapes
C<MT::XS::TiedHash> and C<MT::XS::TiedArray>, C<MT::XS::Proxy>,
C<MT::XS::Proxy::Obj> and C<MT::XS::RemoteCode>.

=cut
*/

void
Perl_mt_xs_boot_bridge(pTHX)
{
    static const char file[] = __FILE__;
    newXS_flags("MT::XS::remote",          XS_MT_XS_remote,          file, "$;$", 0);
    newXS_flags("MT::XS::set_remote_var",  XS_MT_XS_set_remote_var,  file, "$$;$", 0);
    newXS_flags("MT::XS::get_remote_var",  XS_MT_XS_get_remote_var,  file, "$;$", 0);
    newXS_flags("MT::XS::is_handle",       XS_MT_XS_is_handle,       file, "$", 0);
    newXS_flags("MT::XS::handle_compartment", XS_MT_XS_handle_compartment, file, "$", 0);
    newXS_flags("MT::XS::bridge_stats",    XS_MT_XS_bridge_stats,    file, "", 0);

    newXS("MT::XS::TiedHash::FETCH",    XS_MT_XS_TiedHash_FETCH,    file);
    newXS("MT::XS::TiedHash::STORE",    XS_MT_XS_TiedHash_STORE,    file);
    newXS("MT::XS::TiedHash::EXISTS",   XS_MT_XS_TiedHash_EXISTS,   file);
    newXS("MT::XS::TiedHash::DELETE",   XS_MT_XS_TiedHash_DELETE,   file);
    newXS("MT::XS::TiedHash::CLEAR",    XS_MT_XS_TiedHash_CLEAR,    file);
    newXS("MT::XS::TiedHash::FIRSTKEY", XS_MT_XS_TiedHash_FIRSTKEY, file);
    newXS("MT::XS::TiedHash::NEXTKEY",  XS_MT_XS_TiedHash_NEXTKEY,  file);
    newXS("MT::XS::TiedHash::SCALAR",   XS_MT_XS_TiedHash_SCALAR,   file);
    newXS("MT::XS::TiedHash::DESTROY",  XS_MT_XS_noop,              file);
    newXS("MT::XS::TiedHash::UNTIE",    XS_MT_XS_noop,              file);

    newXS("MT::XS::TiedArray::FETCH",     XS_MT_XS_TiedArray_FETCH,     file);
    newXS("MT::XS::TiedArray::STORE",     XS_MT_XS_TiedArray_STORE,     file);
    newXS("MT::XS::TiedArray::FETCHSIZE", XS_MT_XS_TiedArray_FETCHSIZE, file);
    newXS("MT::XS::TiedArray::PUSH",      XS_MT_XS_TiedArray_PUSH,      file);
    newXS("MT::XS::TiedArray::CLEAR",     XS_MT_XS_TiedArray_CLEAR,     file);
    newXS("MT::XS::TiedArray::EXISTS",    XS_MT_XS_TiedArray_EXISTS,    file);
    newXS("MT::XS::TiedArray::DELETE",    XS_MT_XS_TiedArray_DELETE,    file);
    newXS("MT::XS::TiedArray::STORESIZE", XS_MT_XS_Tied_unsupported,    file);
    newXS("MT::XS::TiedArray::POP",       XS_MT_XS_Tied_unsupported,    file);
    newXS("MT::XS::TiedArray::SHIFT",     XS_MT_XS_Tied_unsupported,    file);
    newXS("MT::XS::TiedArray::UNSHIFT",   XS_MT_XS_Tied_unsupported,    file);
    newXS("MT::XS::TiedArray::SPLICE",    XS_MT_XS_Tied_unsupported,    file);
    newXS("MT::XS::TiedArray::EXTEND",    XS_MT_XS_noop,                file);
    newXS("MT::XS::TiedArray::DESTROY",   XS_MT_XS_noop,                file);
    newXS("MT::XS::TiedArray::UNTIE",     XS_MT_XS_noop,                file);

    newXS("MT::XS::Proxy::AUTOLOAD",      XS_MT_XS_Proxy_AUTOLOAD,      file);
    newXS("MT::XS::Proxy::DESTROY",       XS_MT_XS_noop,                file);
    newXS("MT::XS::Proxy::Obj::AUTOLOAD", XS_MT_XS_Proxy_Obj_AUTOLOAD,  file);
    newXS("MT::XS::Proxy::Obj::DESTROY",  XS_MT_XS_noop,                file);
    newXS("MT::XS::Proxy::Obj::MT_class", XS_MT_XS_Proxy_Obj_MT_class,  file);
    newXS("MT::XS::RemoteCode::call",     XS_MT_XS_RemoteCode_call,     file);
    newXS("MT::XS::RemoteCode::DESTROY",  XS_MT_XS_noop,                file);
}

/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

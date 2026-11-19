/*    mt_freeze.c
 *
 *    perl5-mt: the freeze arena and perl_freeze_boot_image().
 *
 *    The boot image is a clone of the running interpreter made into the
 *    arena, a sealable segment from krt: while the clone is
 *    built every Perl_safesys* allocation is a bump allocation from the
 *    arena, so the clone's SV heads and bodies, string buffers, HEKs, hash
 *    entries, pads and GPs all land in sealable memory (with
 *    PERL_MT_IMAGE=1 so do the op slabs, from the start of the process).
 *    The freeze pass then gives every hash an aux structure, normalises
 *    what would otherwise be written on read (copy-on-write counts, shared
 *    hash keys, the mro linearisations), assigns the per-worker table
 *    indices (CvMTID, gp_mt_id, the HV aux id), marks every SV
 *    SVf_IMMORTAL|SVf_READONLY|SVf_PROTECT with a saturated reference
 *    count, marks the op slabs read-only and seals the arena: with a
 *    memory protection key where the OS has them (every thread then
 *    arms it NOWRITE), otherwise with perl_krt_shm_seal_ro.
 *
 *    The parent keeps running on its own, mutable, copy; worker
 *    interpreters (mt_worker.c) are built on the image.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_FREEZE_C
#include "perl.h"
#include "XSUB.h"

#include <sys/mman.h>
#ifdef __APPLE__
#  include <mach/mach.h>
#  include <mach/mach_vm.h>
#endif

#define MT_ARENA_HDR 16   /* bytes in front of each block: its size */

#if defined(__linux__)
static void S_mt_clear_soft_dirty(pTHX);
#endif

/*
=for apidoc_section $embedding

=for apidoc mt_arena_alloc

Bump-allocates C<size> bytes from the freeze arena.  Returns NULL when
there is no arena or it is full (the caller then falls back to the heap).

=for apidoc mt_arena_contains

Whether C<p> points into the freeze arena.

=for apidoc mt_arena_size

The size that was requested for the arena block C<p>.

=cut
*/

void *
Perl_mt_arena_alloc(size_t size)
{
    size_t need;
    char *p;

    if (!PL_mt_arena.base)
        return NULL;
    need = (size + MT_ARENA_HDR + 15) & ~(size_t)15;
    if (need < size || PL_mt_arena_used + need > PL_mt_arena.len) {
        PL_mt_arena_overflow += size;
        return NULL;
    }
    p = (char *)PL_mt_arena.base + PL_mt_arena_used;
    PL_mt_arena_used += need;
    *(size_t *)p = size;
    return p + MT_ARENA_HDR;
}

bool
Perl_mt_arena_contains(const void *p)
{
    PERL_ARGS_ASSERT_MT_ARENA_CONTAINS;
    return PL_mt_arena.base
        && (const char *)p >= (const char *)PL_mt_arena.base
        && (const char *)p <  (const char *)PL_mt_arena.base + PL_mt_arena.len;
}

size_t
Perl_mt_arena_size(const void *p)
{
    PERL_ARGS_ASSERT_MT_ARENA_SIZE;
    return *(const size_t *)((const char *)p - MT_ARENA_HDR);
}

/*
=for apidoc mt_arena_create

Creates the freeze arena of C<size> bytes (0: C<PERL_MT_ARENA_MB> from the
environment, default 512 MiB; the segment is sparse, only the pages that
are touched cost anything).  Returns 0, or a negative errno from krt.

=cut
*/

int
Perl_mt_arena_create(pTHX_ size_t size)
{
    int rc;

    if (PL_mt_arena.base)
        return 0;
    if (!size) {
        const char * const e = PerlEnv_getenv("PERL_MT_ARENA_MB");
        const long mb = e && *e ? atol(e) : 0;
        size = (size_t)(mb > 0 ? mb : 512) << 20;
    }
    rc = perl_krt_shm_create(&PL_mt_arena, size, 0);
    if (rc) {
        Zero(&PL_mt_arena, 1, perl_krt_shm_t);
        PL_mt_arena.fd = PL_mt_arena.ro_fd = -1;
        return rc;
    }
    PL_mt_arena_used = 0;
    PL_mt_arena_overflow = 0;
    return 0;
}

/* ---- Overlay-validated inline caches & freeze specialisation ----------------- */

static U32
S_mt_next_pow2(size_t n)
{
    U32 sz = 16;
    while (sz < n * 2 && sz < (1u << 20))
        sz <<= 1;
    return sz;
}

static U32
S_mt_hash_ptr(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 17;
    v *= (uintptr_t)0xbf58476d1ce4e5b9ULL;
    v ^= v >> 31;
    return (U32)v;
}

static U32
S_mt_sv_hash(SV *sv)
{
    STRLEN len;
    const char *pv;
    U32 h = 0;

    if (!sv || !SvPOKp(sv))
        return 0;
    if (SvIsCOW_shared_hash(sv))
        return SvSHARED_HASH(sv);
    pv = SvPVX_const(sv);
    len = SvCUR(sv);
    PERL_HASH(h, pv, len);
    return h;
}

static SV *
S_mt_op_sv_from_pad(const OP *o, PAD *pad)
{
#ifdef USE_ITHREADS
    if (o->op_type == OP_GV) {
        const PADOFFSET ix = cPADOPx(o)->op_padix;
        if (pad && ix && (SSize_t)ix <= AvFILLp(pad))
            return AvARRAY(pad)[ix];
        return NULL;
    }
    if (o->op_type == OP_METHOD_NAMED) {
        if (cMETHOPx(o)->op_u.op_meth_sv)
            return cMETHOPx(o)->op_u.op_meth_sv;
        if (pad && o->op_targ && (SSize_t)o->op_targ <= AvFILLp(pad))
            return AvARRAY(pad)[o->op_targ];
        return NULL;
    }
    if (cSVOPx(o)->op_sv)
        return cSVOPx(o)->op_sv;
    if (pad && o->op_targ && (SSize_t)o->op_targ <= AvFILLp(pad))
        return AvARRAY(pad)[o->op_targ];
    return NULL;
#else
    PERL_UNUSED_ARG(pad);
    if (o->op_type == OP_METHOD_NAMED)
        return cMETHOPx(o)->op_u.op_meth_sv;
    return cSVOPx(o)->op_sv;
#endif
}

/* Read-only stash lookup in the frozen image (never allocates or mutates). */
static HV *
S_mt_stash_lookup_ro(pTHX_ const char *name, STRLEN len, bool is_utf8)
{
    HV *stash = PL_defstash;
    const char *p = name;
    const char *const end = name + len;

    if (!stash || !name || !len)
        return NULL;
    if (len >= 2 && p[0] == ':' && p[1] == ':')
        p += 2;

    while (p < end) {
        const char *sep = p;
        STRLEN partlen;
        char buf[256];
        HE *he;
        SV *val;

        while (sep < end) {
            if (sep[0] == ':' && sep + 1 < end && sep[1] == ':')
                break;
            sep++;
        }
        partlen = (STRLEN)(sep - p);
        if (partlen == 0 || partlen + 2 >= sizeof(buf))
            return NULL;
        Copy(p, buf, partlen, char);
        buf[partlen]     = ':';
        buf[partlen + 1] = ':';
        he = (HE *)Perl_hv_common(aTHX_ stash, NULL, buf, partlen + 2,
                                  is_utf8 ? HVhek_UTF8 : 0,
                                  HV_MT_RAW, NULL, 0);
        if (!he)
            return NULL;
        val = HeVAL(he);
        if (!val || !isGV_with_GP(val) || !GvGP_raw(val) || !GvGP_raw(val)->gp_hv)
            return NULL;
        stash = GvGP_raw(val)->gp_hv;
        p = (sep < end) ? sep + 2 : end;
    }
    return (SvFLAGS(stash) & SVf_IMMORTAL) && HvHasAUX(stash) ? stash : NULL;
}

/* Read-only MRO method resolution on an immortal stash.  Uses the linear ISA
 * already computed in step 1 and HV_MT_RAW fetches, so nothing is written. */
static bool
S_mt_resolve_method_ro(pTHX_ HV *stash, SV *meth, U32 *gp_id_out, CV **cv_out)
{
    AV *isa;
    SSize_t i, n;
    STRLEN mlen;
    const char *mpv;
    int mflags;

    if (!stash || !(SvFLAGS(stash) & SVf_IMMORTAL) || !HvHasAUX(stash))
        return FALSE;
    if (!meth || !SvPOKp(meth))
        return FALSE;
    mpv = SvPVX_const(meth);
    mlen = SvCUR(meth);
    mflags = SvUTF8(meth) ? HVhek_UTF8 : 0;

    {
        struct mro_meta * const meta = HvAUX_raw(stash)->xhv_mro_meta;
        if (!meta || !meta->mro_linear_current
            || SvTYPE(meta->mro_linear_current) != SVt_PVAV)
            return FALSE;
        isa = (AV *)meta->mro_linear_current;
    }

    n = AvFILLp(isa) + 1;
    for (i = 0; i < n; i++) {
        SV * const pkgsv = AvARRAY(isa)[i];
        HV *cur;
        HE *he;
        SV *val;

        if (!pkgsv || !SvPOKp(pkgsv))
            continue;
        cur = S_mt_stash_lookup_ro(aTHX_ SvPVX_const(pkgsv), SvCUR(pkgsv),
                                   cBOOL(SvUTF8(pkgsv)));
        if (!cur)
            continue;
        he = (HE *)Perl_hv_common(aTHX_ cur, NULL, mpv, mlen, mflags,
                                  HV_MT_RAW, NULL, 0);
        if (!he)
            continue;
        val = HeVAL(he);
        if (!val)
            continue;
        if (isGV_with_GP(val)) {
            GP * const gp = GvGP_raw(val);
            if (gp && !gp->gp_cvgen && gp->gp_cv) {
                CV * const cv = gp->gp_cv;
                if (CvISXSUB(cv) || CvROOT(cv)) {
                    *gp_id_out = gp->gp_mt_id;
                    *cv_out = cv;
                    return TRUE;
                }
                return FALSE; /* stub / AUTOLOAD */
            }
        }
        else if (SvROK(val) && SvTYPE(SvRV(val)) == SVt_PVCV) {
            CV * const cv = MUTABLE_CV(SvRV(val));
            if (CvISXSUB(cv) || CvROOT(cv)) {
                *gp_id_out = U32_MAX;
                *cv_out = cv;
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

/* Detect a provably pure constant-returning frozen CV (e.g. sub FOO { 42 }). */
static SV *
S_mt_pure_const_sv(CV *cv)
{
    OP *root;
    OP *o;
    OP *const_op = NULL;
    PAD *pad;

    if (!cv || CvISXSUB(cv) || CvLVALUE(cv) || CvCLONE(cv) || CvANON(cv))
        return NULL;
    root = CvROOT(cv);
    if (!root || root->op_type != OP_LEAVESUB || !(root->op_flags & OPf_KIDS))
        return NULL;
    if (!CvPADLIST_raw(cv) || PadlistMAX(CvPADLIST_raw(cv)) < 1)
        return NULL;
    pad = PadlistARRAY(CvPADLIST_raw(cv))[1];

    o = cUNOPx(root)->op_first;
    if (o && o->op_type == OP_LINESEQ && (o->op_flags & OPf_KIDS)) {
        OP *kid;
        for (kid = cUNOPx(o)->op_first; kid; kid = OpSIBLING(kid)) {
            if (kid->op_type == OP_NEXTSTATE || kid->op_type == OP_NULL
                || kid->op_type == OP_PUSHMARK)
                continue;
            if (kid->op_type == OP_CONST && !const_op) {
                const_op = kid;
                continue;
            }
            return NULL;
        }
    }
    else if (o && o->op_type == OP_CONST) {
        const_op = o;
    }

    if (!const_op)
        return NULL;
    {
        SV * const sv = S_mt_op_sv_from_pad(const_op, pad);
        if (!sv || !(SvFLAGS(sv) & SVf_IMMORTAL)
            || SvMAGICAL(sv) || SvROK(sv) || SvTYPE(sv) >= SVt_PVAV)
            return NULL;
        return sv;
    }
}

struct mt_spec_walk_ctx {
    struct perl_mt_image *img;
    PTR_TBL_t            *const_cvs;
    SV                  **meth_names;
    size_t                n_meth_names;
    size_t                cap_meth_names;
    struct perl_mt_site_ic *sites;
    size_t                n_sites;
    size_t                cap_sites;
};

static void
S_mt_spec_add_meth(struct mt_spec_walk_ctx *ctx, SV *meth)
{
    size_t i;
    STRLEN len;
    const char *pv;

    if (!meth || !SvPOKp(meth))
        return;
    pv = SvPVX_const(meth);
    len = SvCUR(meth);
    for (i = 0; i < ctx->n_meth_names; i++) {
        SV * const m = ctx->meth_names[i];
        if (m == meth
            || (SvCUR(m) == len && SvUTF8(m) == SvUTF8(meth)
                && memEQ(SvPVX_const(m), pv, len)))
            return;
    }
    if (ctx->n_meth_names == ctx->cap_meth_names) {
        ctx->cap_meth_names = ctx->cap_meth_names ? ctx->cap_meth_names * 2 : 64;
        Renew(ctx->meth_names, ctx->cap_meth_names, SV *);
    }
    ctx->meth_names[ctx->n_meth_names++] = meth;
}

static void
S_mt_spec_add_site(struct mt_spec_walk_ctx *ctx, const struct perl_mt_site_ic *s)
{
    if (ctx->n_sites == ctx->cap_sites) {
        ctx->cap_sites = ctx->cap_sites ? ctx->cap_sites * 2 : 64;
        Renew(ctx->sites, ctx->cap_sites, struct perl_mt_site_ic);
    }
    ctx->sites[ctx->n_sites++] = *s;
}

static void
S_mt_spec_walk_op(pTHX_ OP *o, PAD *pad, struct mt_spec_walk_ctx *ctx)
{
    OP *kid;

    if (!o)
        return;

    if (o->op_type == OP_ENTERSUB
        && !(o->op_private & (OPpENTERSUB_DB | OPpENTERSUB_LVAL_MASK))
        && !(o->op_flags & OPf_MOD)
        && (o->op_flags & OPf_KIDS))
    {
        OP *first = cUNOPo->op_first;
        if (first && first->op_type == OP_NULL && (first->op_flags & OPf_KIDS))
            first = cUNOPx(first)->op_first;

        if (first && first->op_type == OP_PUSHMARK) {
            OP *arg1 = OpSIBLING(first);
            /* Case 1: 0-arg scalar call to a pure constant frozen sub:
             *   pushmark -> ex-rv2cv(gv) -> entersub */
            if ((o->op_flags & OPf_WANT) == OPf_WANT_SCALAR
                && arg1 && !OpSIBLING(arg1))
            {
                OP *gvop = arg1;
                if (gvop->op_type == OP_NULL && (gvop->op_flags & OPf_KIDS))
                    gvop = cUNOPx(gvop)->op_first;
                if (gvop && gvop->op_type == OP_GV && gvop->op_next == o) {
                    SV * const gsv = S_mt_op_sv_from_pad(gvop, pad);
                    if (gsv && isGV_with_GP(gsv)
                        && (SvFLAGS(gsv) & SVf_IMMORTAL)
                        && GvGP_raw(gsv) && !GvGP_raw(gsv)->gp_cvgen
                        && GvGP_raw(gsv)->gp_cv)
                    {
                        SV * const csv = (SV *)ptr_table_fetch(
                            ctx->const_cvs, GvGP_raw(gsv)->gp_cv);
                        if (csv) {
                            struct perl_mt_site_ic site;
                            Zero(&site, 1, struct perl_mt_site_ic);
                            site.op    = o;
                            site.kind  = MT_SITE_CONST_SUB;
                            site.gp_id = GvGP_raw(gsv)->gp_mt_id;
                            site.sv    = csv;
                            S_mt_spec_add_site(ctx, &site);
                            ctx->img->n_spec_const_subs++;
                        }
                    }
                }
            }
            /* Case 2: static class method call:
             *   pushmark -> const(Class) -> ... -> method_named(meth) -> entersub */
            else if (arg1 && arg1->op_type == OP_CONST) {
                OP *last = arg1;
                while (OpSIBLING(last))
                    last = OpSIBLING(last);
                if (last->op_type == OP_METHOD_NAMED && last->op_next == o) {
                    SV * const cls  = S_mt_op_sv_from_pad(arg1, pad);
                    SV * const meth = S_mt_op_sv_from_pad(last, pad);
                    if (cls && SvPOKp(cls) && meth && SvPOKp(meth)) {
                        HV * const stash = S_mt_stash_lookup_ro(
                            aTHX_ SvPVX_const(cls), SvCUR(cls),
                            cBOOL(SvUTF8(cls)));
                        U32 gp_id = U32_MAX;
                        CV *target_cv = NULL;
                        if (stash
                            && S_mt_resolve_method_ro(aTHX_ stash, meth,
                                                      &gp_id, &target_cv))
                        {
                            struct perl_mt_site_ic site;
                            Zero(&site, 1, struct perl_mt_site_ic);
                            site.op       = last;
                            site.kind     = MT_SITE_METH_DEVIRT;
                            site.stash_id = HvMT_ID(HvAUX_raw(stash));
                            site.gp_id    = gp_id;
                            site.stash    = stash;
                            site.sv       = (SV *)target_cv;
                            site.class_sv = cls;
                            S_mt_spec_add_site(ctx, &site);
                            ctx->img->n_spec_meth_devirt++;
                        }
                    }
                }
            }
        }
    }
    else if (o->op_type == OP_METHOD_NAMED) {
        SV * const meth = S_mt_op_sv_from_pad(o, pad);
        if (meth)
            S_mt_spec_add_meth(ctx, meth);
    }

    if (o->op_flags & OPf_KIDS) {
        for (kid = cUNOPo->op_first; kid; kid = OpSIBLING(kid))
            S_mt_spec_walk_op(aTHX_ kid, pad, ctx);
    }
}

static void
S_mt_freeze_specialize(pTHX_ struct perl_mt_image *img)
{
    const char * const no_spec = PerlEnv_getenv("PERL_MT_NO_SPECIALIZE");
    struct mt_spec_walk_ctx ctx;
    U32 i;

    if (no_spec && *no_spec && strNE(no_spec, "0")) {
        img->specialize_enabled = FALSE;
        return;
    }
    img->specialize_enabled = TRUE;

    Zero(&ctx, 1, struct mt_spec_walk_ctx);
    ctx.img = img;
    ctx.const_cvs = ptr_table_new();

    /* 1. Identify pure constant frozen subs */
    for (i = 0; i < img->ncv; i++) {
        CV * const cv = img->cvs[i];
        SV * const csv = S_mt_pure_const_sv(cv);
        if (csv)
            ptr_table_store(ctx.const_cvs, cv, csv);
    }

    /* 2. Walk frozen optrees for call-site specialisations & method names */
    for (i = 0; i < img->ncv; i++) {
        CV * const cv = img->cvs[i];
        if (!CvISXSUB(cv) && CvROOT(cv)) {
            PADLIST * const pl = CvPADLIST_raw(cv);
            PAD * const pad = (pl && PadlistMAX(pl) >= 1)
                              ? PadlistARRAY(pl)[1] : NULL;
            S_mt_spec_walk_op(aTHX_ CvROOT(cv), pad, &ctx);
        }
    }
    if (PL_main_root) {
        PADLIST * const pl = PL_main_cv ? CvPADLIST_raw(PL_main_cv) : NULL;
        PAD * const pad = (pl && PadlistMAX(pl) >= 1)
                          ? PadlistARRAY(pl)[1] : NULL;
        S_mt_spec_walk_op(aTHX_ PL_main_root, pad, &ctx);
    }

    /* 3. Build open-addressed site_ic table */
    if (ctx.n_sites > 0) {
        const U32 sz = S_mt_next_pow2(ctx.n_sites);
        const U32 mask = sz - 1;
        size_t j;
        Newxz(img->site_ic, sz, struct perl_mt_site_ic);
        img->site_ic_mask = mask;
        for (j = 0; j < ctx.n_sites; j++) {
            U32 slot = S_mt_hash_ptr(ctx.sites[j].op) & mask;
            while (img->site_ic[slot].op)
                slot = (slot + 1) & mask;
            img->site_ic[slot] = ctx.sites[j];
        }
    }

    /* 4. Build overlay-validated method IC over all frozen stashes x method names */
    if (ctx.n_meth_names > 0) {
        struct perl_mt_meth_ic *tmp = NULL;
        size_t n_tmp = 0, cap_tmp = 0;
        size_t m;

        for (i = 0; i < img->nhv; i++) {
            HV * const hv = img->hvs[i];
            if (!hv || !HvHasNAME(hv))
                continue;
            for (m = 0; m < ctx.n_meth_names; m++) {
                SV * const meth = ctx.meth_names[m];
                U32 gp_id = U32_MAX;
                CV *cv = NULL;
                if (S_mt_resolve_method_ro(aTHX_ hv, meth, &gp_id, &cv)) {
                    if (n_tmp == cap_tmp) {
                        cap_tmp = cap_tmp ? cap_tmp * 2 : 128;
                        Renew(tmp, cap_tmp, struct perl_mt_meth_ic);
                    }
                    tmp[n_tmp].stash     = hv;
                    tmp[n_tmp].meth      = meth;
                    tmp[n_tmp].cv        = cv;
                    tmp[n_tmp].stash_id  = HvMT_ID(HvAUX_raw(hv));
                    tmp[n_tmp].gp_id     = gp_id;
                    tmp[n_tmp].meth_hash = S_mt_sv_hash(meth);
                    n_tmp++;
                }
            }
        }
        if (n_tmp > 0) {
            const U32 sz = S_mt_next_pow2(n_tmp);
            const U32 mask = sz - 1;
            size_t j;
            Newxz(img->meth_ic, sz, struct perl_mt_meth_ic);
            img->meth_ic_mask = mask;
            img->n_meth_ic = n_tmp;
            for (j = 0; j < n_tmp; j++) {
                U32 slot = (S_mt_hash_ptr(tmp[j].stash) ^ tmp[j].meth_hash) & mask;
                while (img->meth_ic[slot].stash)
                    slot = (slot + 1) & mask;
                img->meth_ic[slot] = tmp[j];
            }
        }
        Safefree(tmp);
    }

    /* 5. Build overlay-validated constant-key hash IC for non-stash frozen hashes */
    {
        struct perl_mt_helem_ic *tmp = NULL;
        size_t n_tmp = 0, cap_tmp = 0;

        for (i = 0; i < img->nhv; i++) {
            HV * const hv = img->hvs[i];
            HE **array;
            STRLEN b;
            U32 hid;

            if (!hv || HvHasNAME(hv) || SvRMAGICAL(hv) || hv == PL_strtab)
                continue;
            if (!HvARRAY(hv) || HvKEYS(hv) == 0 || HvKEYS(hv) > 2048)
                continue;
            if (n_tmp > 65536)
                break;
            hid = HvMT_ID(HvAUX_raw(hv));
            array = HvARRAY(hv);
            for (b = 0; b <= HvMAX(hv); b++) {
                HE *he;
                for (he = array[b]; he; he = HeNEXT(he)) {
                    HEK * const hek = HeKEY_hek(he);
                    SV  * const val = HeVAL(he);
                    if (!hek || !val || val == &PL_sv_placeholder)
                        continue;
                    if (n_tmp == cap_tmp) {
                        cap_tmp = cap_tmp ? cap_tmp * 2 : 256;
                        Renew(tmp, cap_tmp, struct perl_mt_helem_ic);
                    }
                    tmp[n_tmp].hv       = hv;
                    tmp[n_tmp].hek      = hek;
                    tmp[n_tmp].val      = val;
                    tmp[n_tmp].hv_id    = hid;
                    tmp[n_tmp].key_hash = HEK_HASH(hek);
                    n_tmp++;
                }
            }
        }
        if (n_tmp > 0) {
            const U32 sz = S_mt_next_pow2(n_tmp);
            const U32 mask = sz - 1;
            size_t j;
            Newxz(img->helem_ic, sz, struct perl_mt_helem_ic);
            img->helem_ic_mask = mask;
            img->n_helem_ic = n_tmp;
            for (j = 0; j < n_tmp; j++) {
                U32 slot = (S_mt_hash_ptr(tmp[j].hv) ^ tmp[j].key_hash) & mask;
                while (img->helem_ic[slot].hv)
                    slot = (slot + 1) & mask;
                img->helem_ic[slot] = tmp[j];
            }
        }
        Safefree(tmp);
    }

    ptr_table_free(ctx.const_cvs);
    Safefree(ctx.meth_names);
    Safefree(ctx.sites);
}

/* ---- the freeze pass, run with the image as the current interpreter ---- */

#define MT_FOR_EACH_SV(sva, sv, body)                                   \
    for (sva = PL_sv_arenaroot; sva; sva = MUTABLE_SV(SvANY(sva))) {    \
        const SV * const svend_ = &sva[SvREFCNT(sva)];                  \
        for (sv = sva + 1; sv < svend_; ++sv) {                         \
            if (SvTYPE(sv) == (svtype)SVTYPEMASK || !SvREFCNT(sv))      \
                continue;                                               \
            body                                                        \
        }                                                               \
    }

static int
S_mt_freeze_pass(pTHX_ struct perl_mt_image *img)
{
    SV *sva;
    SV *sv;
    PTR_TBL_t *gpt;
    U32 i;

    /* 1. things that allocate into the image: every hash gets an aux
     *    (a worker's iteration of a frozen hash never allocates one),
     *    copy-on-write strings are normalised, the mro linearisations of
     *    every stash are computed */
    PL_mt_arena_mode = 1;
    MT_FOR_EACH_SV(sva, sv, {
        switch (SvTYPE(sv)) {
        case SVt_PVHV:
            if ((HV *)sv == PL_strtab)
                break;
            if (!HvHasAUX(sv))
                hv_iterinit((HV *)sv);
            if (HvHasNAME((HV *)sv))
                (void)mro_get_linear_isa((HV *)sv);
            break;
        case SVt_PVAV: case SVt_PVCV: case SVt_PVFM: case SVt_PVIO:
        case SVt_REGEXP:
            break;
        default:
            if (SvPOKp(sv) && SvIsCOW(sv) && !isGV_with_GP(sv)) {
                if (SvLEN(sv) == 0) {
                    /* a shared hash key string: give it a buffer of its
                     * own (the program's own read-only constants are
                     * among these, so the check is lifted for the copy) */
                    const U32 ro = SvFLAGS(sv) & (SVf_READONLY|SVf_PROTECT);
                    SvFLAGS(sv) &= ~ro;
                    sv_force_normal_flags(sv, 0);
                    SvFLAGS(sv) |= ro;
                }
                else if (!(SvFLAGS(sv) & SVppv_STATIC))
                    CowREFCNT(sv) = SV_COW_REFCNT_MAX; /* copies forced */
            }
            break;
        }
    });
    PL_mt_arena_mode = 0;

    /* 2. no hash key of the image is ever shared with a worker's string
     *    table: newSVhek and friends copy a NOTSHARED key */
    if (PL_strtab && HvARRAY(PL_strtab)) {
        HE **array = HvARRAY(PL_strtab);
        STRLEN b;
        for (b = 0; b <= HvMAX(PL_strtab); b++) {
            HE *he;
            for (he = array[b]; he; he = HeNEXT(he))
                HEK_FLAGS(HeKEY_hek(he)) |= HVhek_NOTSHARED;
        }
    }

    /* 3. count and assign the per-worker table indices */
    gpt = ptr_table_new();
    img->n_sv = img->ncv = img->ngp = img->nhv = 0;
    MT_FOR_EACH_SV(sva, sv, {
        img->n_sv++;
        switch (SvTYPE(sv)) {
        case SVt_PVCV: case SVt_PVFM:
            CvMTID(sv) = img->ncv++;
            break;
        case SVt_PVGV: case SVt_PVLV:
            if (isGV_with_GP(sv) && GvGP_raw(sv)) {
                GP * const gp = GvGP_raw(sv);
                if (!ptr_table_fetch(gpt, gp)) {
                    ptr_table_store(gpt, gp, (void *)(Size_t)(img->ngp + 1));
                    gp->gp_mt_id = img->ngp++;
                }
            }
            break;
        case SVt_PVHV:
            if (HvHasAUX(sv)) {
                struct xpvhv_aux * const aux = HvAUX_raw(sv);
                if (img->nhv >= (1u << (32 - HvAUXf_MT_ID_SHIFT))) {
                    ptr_table_free(gpt);
                    return MT_FREEZE_FAILED;
                }
                aux->xhv_aux_flags = (aux->xhv_aux_flags & HvAUXf_MT_MASK)
                                   | (img->nhv << HvAUXf_MT_ID_SHIFT);
                img->nhv++;
            }
            break;
        default:
            break;
        }
    });
    Newxz(img->cvs, img->ncv ? img->ncv : 1, CV *);
    Newxz(img->gps, img->ngp ? img->ngp : 1, GP *);
    Newxz(img->gvs, img->ngp ? img->ngp : 1, GV *);
    Newxz(img->hvs, img->nhv ? img->nhv : 1, HV *);

    /* 4. fill the tables and mark everything immortal */
    MT_FOR_EACH_SV(sva, sv, {
        switch (SvTYPE(sv)) {
        case SVt_PVCV: case SVt_PVFM:
            img->cvs[CvMTID(sv)] = (CV *)sv;
            break;
        case SVt_PVGV: case SVt_PVLV:
            if (isGV_with_GP(sv) && GvGP_raw(sv)) {
                GP * const gp = GvGP_raw(sv);
                img->gps[gp->gp_mt_id] = gp;
                if (!img->gvs[gp->gp_mt_id])
                    img->gvs[gp->gp_mt_id] = (GV *)sv;
            }
            break;
        case SVt_PVHV:
            if (HvHasAUX(sv))
                img->hvs[HvMT_ID(HvAUX_raw(sv))] = (HV *)sv;
            break;
        default:
            break;
        }
        SvFLAGS(sv) |= SVf_IMMORTAL|SVf_READONLY|SVf_PROTECT;
        SvREFCNT(sv) = SvREFCNT_IMMORTAL;
    });
    ptr_table_free(gpt);

    /* 4b. Build overlay-validated inline caches and run
     *     freeze-time specialisation (skipped if PERL_MT_NO_SPECIALIZE=1) */
    img->base_sub_gen = PL_sub_generation;
    S_mt_freeze_specialize(aTHX_ img);

    /* 5. the roots, and the op slabs become read-only */
    img->defstash    = PL_defstash;
    img->main_root   = PL_main_root;
    img->main_start  = PL_main_start;
    img->main_cv     = PL_main_cv;
    img->regex_padav = PL_regex_padav;
    for (i = 0; i < img->ncv; i++) {
        CV * const cv = img->cvs[i];
        if (!CvISXSUB(cv) && CvROOT(cv) && CvROOT(cv)->op_slabbed)
            Slab_to_ro(OpSLAB(CvROOT(cv)));
    }
    if (PL_main_root && PL_main_root->op_slabbed)
        Slab_to_ro(OpSLAB(PL_main_root));
    return 0;
}

/*
=for apidoc freeze_boot_image

Builds and seals the boot image (see the description at the top of
F<mt_freeze.c>); C<perl_freeze_boot_image(aTHX)> is the same call.  Dies
with C<MT_ALREADY_FROZEN> on a second call and C<MT_FREEZE_FAILED> when
the arena cannot be created.

=cut
*/

void
Perl_freeze_boot_image(pTHX)
{
    PerlInterpreter * const parent = aTHX;
    PerlInterpreter *image;
    struct perl_mt_image *img;
    perl_krt_key_t key = PERL_KRT_KEY_NONE;

    if (PL_mt_image)
        Perl_croak_mt(aTHX_ MT_ALREADY_FROZEN);
    /* the XS compartments are forked now, from a single-threaded process
     * that does not yet carry the image (mt_xs_compartment.c) */
    Perl_mt_xs_pool_start(aTHX);
    if (Perl_mt_arena_create(aTHX_ 0) != 0)
        Perl_croak_mt(aTHX_ MT_FREEZE_FAILED);

    /* an XS module with MY_CXT but no CLONE would hand every worker the
     * image's sealed copy of its state: refused, unless vouched for */
    {
        HV * const ok = get_hv("MT::XS::inprocess_ok", 0);
        U32 i;
        for (i = 0; i < PL_mt_my_cxt_npkgs; i++) {
            const char * const pkg = PL_mt_my_cxt_pkgs[i];
            HV * const stash = gv_stashpv(pkg, 0);
            if (ok && hv_exists(ok, pkg, (I32)strlen(pkg)))
                continue;
            if (stash && gv_fetchmeth_pvn(stash, "CLONE", 5, 0, 0))
                continue;
            Perl_croak(aTHX_ "perl5-mt: XS module %s uses MY_CXT without a "
                             "CLONE method (MT_MY_CXT_WITHOUT_CLONE)", pkg);
        }
    }

    Newxz(img, 1, struct perl_mt_image);
    img->ops_in_arena = cBOOL(PL_mt_ops_in_arena);
    /* the capability table exists before any worker does (mt_cap.c) */
    Perl_mt_cap_init(aTHX);
    PL_mt_ops_in_arena = 0;           /* anything compiled later: heap */

    /* 1. the clone, built in the arena (what perl_clone still does in the
     *    parent's context, the CLONE_SKIP lookups, stays on the heap) */
    PL_mt_arena_exclude = parent;
    PL_mt_arena_mode = 1;
    image = perl_clone(parent, CLONEf_KEEP_PTR_TABLE);
    PL_mt_arena_mode = 0;
    PL_mt_arena_exclude = NULL;
    img->interp = image;
    img->clone_stashes = image->Imt_clone_stashes;

    /* 2. the freeze pass, in the image's own context (perl_clone has
     *    already made it the current interpreter) */
    {
        const int err = S_mt_freeze_pass(image, img);
        PERL_SET_CONTEXT(parent);
        if (err)
            Perl_croak_mt(aTHX_ err);
    }

    /* 3. seal: a protection key where the OS has them, armed NOWRITE in
     *    this thread before any worker exists (threads inherit it) and
     *    re-armed at every JMPENV_PUSH landing; the plain seal otherwise */
    if (perl_krt_key_alloc(&key) == 0) {
        if (perl_krt_shm_bind_key(&PL_mt_arena, key) == 0
            && perl_krt_shm_seal_size(&PL_mt_arena) == 0
            && perl_krt_key_arm(key, PERL_KRT_KEY_NOWRITE) == 0) {
            PL_mt_image_key = key;
            PL_mt_image_key_armed = 1;
            img->sealed = TRUE;
        }
        else {
            (void)perl_krt_key_free(key);
            key = PERL_KRT_KEY_NONE;
        }
    }
    if (!img->sealed
        && perl_krt_shm_seal_ro(&PL_mt_arena, PL_mt_arena.base,
                                PL_mt_arena.len) == 0)
        img->sealed = TRUE;

    img->arena = PL_mt_arena;
    img->arena_used = PL_mt_arena_used;
#if defined(__linux__)
    /* from now on a soft-dirty page of the arena is one somebody wrote to */
    S_mt_clear_soft_dirty(aTHX);
#endif
    PL_mt_image = img;
}

/* ---- statistics ---------------------------------------------------------- */

#if defined(__linux__)
/* a line reader for the procfs files (PerlIO has no gets) */
static bool
S_mt_getline(pTHX_ PerlIO *f, char *line, size_t max)
{
    size_t n = 0;
    int c;
    while ((c = PerlIO_getc(f)) != EOF) {
        if (n + 1 < max)
            line[n++] = (char)c;
        if (c == '\n')
            break;
    }
    line[n] = '\0';
    return n > 0;
}

/* Linux soft-dirty tracking: the bits are cleared when the image is
 * sealed; afterwards a page of the arena with the bit set has been written
 * to, whatever its protection.  -1 when unavailable. */
static void
S_mt_clear_soft_dirty(pTHX)
{
    PerlIO * const f = PerlIO_open("/proc/self/clear_refs", "w");
    if (f) {
        PerlIO_write(f, "4\n", 2);
        PerlIO_close(f);
    }
}

static IV
S_mt_image_written_kb(pTHX_ const struct perl_mt_image *img)
{
    const size_t page = perl_krt_page_size();
    const size_t npages = img->arena.len / page;
    const Off_t first = (Off_t)((uintptr_t)img->arena.base / page);
    PerlIO * const f = PerlIO_open("/proc/self/pagemap", "r");
    IV kb = 0;
    size_t i;

    if (!f)
        return -1;
    if (PerlIO_seek(f, first * (Off_t)sizeof(U64), SEEK_SET) < 0) {
        PerlIO_close(f);
        return -1;
    }
    for (i = 0; i < npages; i++) {
        U64 ent;
        if (PerlIO_read(f, &ent, sizeof ent) != (SSize_t)sizeof ent)
            break;
        if (ent & ((U64)1 << 55))         /* soft-dirty */
            kb += (IV)(page / 1024);
    }
    PerlIO_close(f);
    return kb;
}
#endif

/* Dirty pages of the image mapping, in kB: Linux reads /proc/self/smaps
 * (for a memfd every resident page counts as dirty, so this is the
 * resident size of the image), Darwin asks the VM for the region; -1
 * when unknown. */
static IV
S_mt_image_dirty_kb(pTHX_ const struct perl_mt_image *img)
{
#if defined(__linux__)
    PerlIO *f;
    char line[512];
    char want[40];
    bool in = FALSE;
    IV kb = 0;
    bool seen = FALSE;

    f = PerlIO_open("/proc/self/smaps", "r");
    if (!f)
        return -1;
    (void)snprintf(want, sizeof want, "%lx-", (unsigned long)img->arena.base);
    while (S_mt_getline(aTHX_ f, line, sizeof line)) {
        const char *dash = strchr(line, '-');
        const char *sp = strchr(line, ' ');
        if (isXDIGIT(line[0]) && dash && sp && dash < sp) {
            in = strncmp(line, want, strlen(want)) == 0;
            if (in)
                seen = TRUE;
            continue;
        }
        if (in && (strnEQ(line, "Private_Dirty:", 14)
                   || strnEQ(line, "Shared_Dirty:", 13)))
            kb += atol(strchr(line, ':') + 1);
    }
    PerlIO_close(f);
    return seen ? kb : -1;
#elif defined(__APPLE__)
    mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)img->arena.base;
    mach_vm_size_t size = 0;
    vm_region_extended_info_data_t info;
    mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t obj = MACH_PORT_NULL;
    IV kb = 0;
    const size_t page = perl_krt_page_size();

    while (addr < (mach_vm_address_t)(uintptr_t)img->arena.base + img->arena.len) {
        if (mach_vm_region(mach_task_self(), &addr, &size,
                           VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info,
                           &count, &obj) != KERN_SUCCESS)
            return kb ? kb : -1;
        if (addr >= (mach_vm_address_t)(uintptr_t)img->arena.base + img->arena.len)
            break;
        kb += (IV)info.pages_dirtied * (IV)(page / 1024);
        addr += size;
    }
    return kb;
#else
    PERL_UNUSED_ARG(img);
    return -1;
#endif
}

/*
=for apidoc mt_image_stats

Returns a new hash reference describing the boot image, for
C<MT::Image::stats>: whether it is frozen and sealed, how it is sealed,
the arena size and use, the counts of SVs, CVs, GPs and hashes, and the
dirty kilobytes of the image mapping.

=cut
*/

SV *
Perl_mt_image_stats(pTHX)
{
    HV * const hv = newHV();
    const struct perl_mt_image * const img = PL_mt_image;

    (void)hv_stores(hv, "frozen", newSViv(img ? 1 : 0));
    (void)hv_stores(hv, "arena_bytes", newSVuv(PL_mt_arena.len));
    (void)hv_stores(hv, "arena_used", newSVuv(PL_mt_arena_used));
    (void)hv_stores(hv, "arena_overflow", newSVuv(PL_mt_arena_overflow));
    (void)hv_stores(hv, "keys", newSViv(PL_mt_image_key >= 0 ? 1 : 0));
    (void)hv_stores(hv, "ops_in_arena",
                    newSViv(img ? img->ops_in_arena : PL_mt_ops_in_arena));
    if (img) {
        (void)hv_stores(hv, "sealed", newSViv(img->sealed ? 1 : 0));
        (void)hv_stores(hv, "specialize",
                        newSViv(img->specialize_enabled ? 1 : 0));
        (void)hv_stores(hv, "n_sv", newSVuv(img->n_sv));
        (void)hv_stores(hv, "n_cv", newSVuv(img->ncv));
        (void)hv_stores(hv, "n_gp", newSVuv(img->ngp));
        (void)hv_stores(hv, "n_hv", newSVuv(img->nhv));
        (void)hv_stores(hv, "n_meth_ic", newSVuv(img->n_meth_ic));
        (void)hv_stores(hv, "n_helem_ic", newSVuv(img->n_helem_ic));
        (void)hv_stores(hv, "n_spec_const_subs",
                        newSVuv(img->n_spec_const_subs));
        (void)hv_stores(hv, "n_spec_meth_devirt",
                        newSVuv(img->n_spec_meth_devirt));
        (void)hv_stores(hv, "dirty_kb", newSViv(S_mt_image_dirty_kb(aTHX_ img)));
#if defined(__linux__)
        (void)hv_stores(hv, "written_kb",
                        newSViv(S_mt_image_written_kb(aTHX_ img)));
#else
        (void)hv_stores(hv, "written_kb", newSViv(-1));
#endif
    }
    return newRV_noinc((SV *)hv);
}

/* ---- MT::Image --------------------------------------------------------- */

XS(XS_MT_Image_freeze);
XS(XS_MT_Image_freeze)
{
#ifdef PERL_RC_STACK
    dMARK;
    rpp_popfree_to_NN(MARK);
    Perl_freeze_boot_image(aTHX);
    rpp_xpush_IMM(&PL_sv_yes);
#else
    dXSARGS;
    PERL_UNUSED_VAR(items);
    Perl_freeze_boot_image(aTHX);
    XSRETURN_YES;
#endif
}

XS(XS_MT_Image_frozen);
XS(XS_MT_Image_frozen)
{
#ifdef PERL_RC_STACK
    dMARK;
    rpp_popfree_to_NN(MARK);
    rpp_xpush_IMM(PL_mt_image ? &PL_sv_yes : &PL_sv_no);
#else
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = PL_mt_image ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
#endif
}

XS(XS_MT_Image_stats);
XS(XS_MT_Image_stats)
{
#ifdef PERL_RC_STACK
    dMARK;
    rpp_popfree_to_NN(MARK);
    rpp_extend(1);
    *++PL_stack_sp = Perl_mt_image_stats(aTHX);
#else
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(Perl_mt_image_stats(aTHX));
    XSRETURN(1);
#endif
}

void
Perl_boot_core_mt(pTHX)
{
    CV *cv;
    cv = newXS_flags("MT::Image::freeze", XS_MT_Image_freeze, __FILE__, "", 0);
#ifdef PERL_RC_STACK
    CvXS_RCSTACK_on(cv);
#endif
    cv = newXS_flags("MT::Image::frozen", XS_MT_Image_frozen, __FILE__, "", 0);
#ifdef PERL_RC_STACK
    CvXS_RCSTACK_on(cv);
#endif
    cv = newXS_flags("MT::Image::stats",  XS_MT_Image_stats,  __FILE__, "", 0);
#ifdef PERL_RC_STACK
    CvXS_RCSTACK_on(cv);
#endif
    PERL_UNUSED_VAR(cv);
    Perl_mt_boot_worker(aTHX);
    Perl_mt_boot_cap(aTHX);
    Perl_mt_boot_xs(aTHX);
}

/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

/*    mt_worker.c
 *
 *    perl5-mt: worker interpreters that run on top of the sealed boot
 *    image, and the per-worker tables that keep every mutable per-CV
 *    datum (recursion depth, padlist) out of the shared, read-only CV
 *    bodies.
 *
 *    Provides the per-worker tables and perl_mt_worker_new().
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_WORKER_C
#include "perl.h"
#include "mt_ctx.h"
#include "mt_fault.h"
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__) && defined(__linux__)
#  include <fenv.h>
#  include <sys/syscall.h>
#endif

/*
=for apidoc_section $CV

=for apidoc mt_cv_depth_p

Returns the address of the recursion depth of the immortal CV C<cv> in the
current worker.  Called through C<CvDEPTH> (L<perlapi/CvDEPTH>) for CVs in
the boot image; never for ordinary CVs.

=cut
*/

I32 *
Perl_mt_cv_depth_p(const CV *cv)
{
    dTHX;
    PERL_ARGS_ASSERT_MT_CV_DEPTH_P;

    if (UNLIKELY(!PL_mt_cv_depth))
        Perl_croak_mt(aTHX_ MT_NOT_A_WORKER);
    assert(CvMTID(cv) < PL_mt_ncv);
    return &PL_mt_cv_depth[CvMTID(cv)];
}

/*
=for apidoc mt_padlist_p

Returns the address of the padlist slot of the immortal CV C<cv> in the
current worker, materialising the padlist from the frozen template on
first use.  Called through C<CvPADLIST> for CVs in the boot image.

=cut
*/

PADLIST **
Perl_mt_padlist_p(const CV *cv)
{
    dTHX;
    PADLIST **slot;
    PERL_ARGS_ASSERT_MT_PADLIST_P;

    if (UNLIKELY(!PL_mt_padlists))
        Perl_croak_mt(aTHX_ MT_NOT_A_WORKER);
    assert(CvMTID(cv) < PL_mt_ncv);
    slot = &PL_mt_padlists[CvMTID(cv)];
    if (!*slot)
        *slot = Perl_mt_padlist_materialize(aTHX_ cv);
    return slot;
}

/*
=for apidoc mt_padlist_materialize

Builds this worker's depth-1 pad for the immortal CV C<cv> from the
frozen template padlist, with the same rules C<pad_push> applies to a
recursive call: C<my> variables and temporaries are fresh, while
constants, C<state> variables, outer lexicals and inner subs refer to the
immortal SVs of the template.  The pad name list is shared with the
image (it is never written to and never reference counted here).

=cut
*/

PADLIST *
Perl_mt_padlist_materialize(pTHX_ const CV *cv)
{
    const PADLIST *tpl;
    PADLIST *pl;
    PAD **tsvp;
    PADNAMELIST *names;
    PADNAME **namepv;
    PADOFFSET names_fill, ix;
    SV **oldpad;
    AV *newpad;
    AV *argav;

    PERL_ARGS_ASSERT_MT_PADLIST_MATERIALIZE;

    tpl = CvPADLIST_raw(cv);
    if (!tpl)
        return NULL;

    tsvp = PadlistARRAY(tpl);
    names = (PADNAMELIST *)tsvp[0];
    oldpad = AvARRAY(tsvp[1]);
    ix = AvFILLp(tsvp[1]);
    names_fill = PadnamelistMAX(names);
    namepv = PadnamelistARRAY(names);

    Newx(pl, 1, PADLIST);
    PadlistMAX(pl) = 1;
    Newxz(PadlistARRAY(pl), 2, PAD *);
    PadlistARRAY(pl)[0] = (PAD *)names;
    pl->xpadl_id = tpl->xpadl_id;
    pl->xpadl_outid = tpl->xpadl_outid;

    newpad = newAV();
    Newxz(AvALLOC(newpad), ix + 1, SV *);
    AvARRAY(newpad) = AvALLOC(newpad);
    AvMAX(newpad) = AvFILLp(newpad) = ix;

    for ( ; ix > 0; ix--) {
        SV *sv;
        /* an unnamed slot (a temporary) has no PADNAME at all */
        PADNAME * const pn = names_fill >= ix ? namepv[ix] : NULL;
        if (pn && PadnameLEN(pn)) {
            const char sigil = PadnamePV(pn)[0];
            if (PadnameOUTER(pn)
                    || PadnameIsSTATE(pn)
                    || sigil == '&')
            {
                SV *tmp = oldpad[ix];
                if (sigil == '&' && tmp && SvTYPE(tmp) == SVt_PVCV
                    && !PadnameOUTER(pn)
                    && CvLEXICAL(tmp) && CvCLONED(tmp)
                    && !PadnameIsOUR(pn)
                    && !PadnameIsSTATE(pn)) {
                    /* lexical sub that needs cloning */
                    sv = newSV_type(SVt_PVCV);
                }
                else {
                    /* outer lexical, state var or inner sub: the
                     * template's immortal SV, shared */
                    sv = SvREFCNT_inc(tmp);
                }
            }
            else {		/* our own lexical */
                if (sigil == '@')
                    sv = MUTABLE_SV(newAV());
                else if (sigil == '%')
                    sv = MUTABLE_SV(newHV());
                else
                    sv = newSV_type(SVt_NULL);
            }
        }
        else if (pn && PadnamePV(pn)) {
            /* constants and the like: shared with the image */
            sv = SvREFCNT_inc_NN(oldpad[ix]);
        }
        else {
            /* temporaries are always fresh */
            sv = newSV_type(SVt_NULL);
            SvPADTMP_on(sv);
        }
        AvARRAY(newpad)[ix] = sv;
    }
    argav = newAV();
    AvARRAY(newpad)[0] = MUTABLE_SV(argav);
#ifndef PERL_RC_STACK
    AvREIFY_only(argav);
#endif
    PadlistARRAY(pl)[1] = newpad;
    return pl;
}

/*
=for apidoc mt_padlist_free

Frees a padlist built by L</mt_padlist_materialize> (or grown from it by
C<pad_push>): every pad is released, the shared name list is left alone.

=cut
*/

void
Perl_mt_padlist_free(pTHX_ PADLIST *padlist)
{
    SSize_t ix;
    PERL_ARGS_ASSERT_MT_PADLIST_FREE;

    for (ix = PadlistMAX(padlist); ix > 0; ix--) {
        PAD *pad = PadlistARRAY(padlist)[ix];
        if (pad)
            SvREFCNT_dec(MUTABLE_SV(pad));
    }
    Safefree(PadlistARRAY(padlist));
    Safefree(padlist);
}

/*
=for apidoc mt_cv_shareable

Returns true when C<cv> can be handed to another worker: it is a member of
the boot image, or a named sub that captures nothing (anonymous subs that
close over lexicals, C<CvCLONE>/C<CvCLONED>, are refused with
C<MT_CLOSURE_NOT_SHARED> by the callers).

=cut
*/

bool
Perl_mt_cv_shareable(const CV *cv)
{
    PERL_ARGS_ASSERT_MT_CV_SHAREABLE;

    if (SvFLAGS(cv) & SVf_IMMORTAL)
        return TRUE;
    if (CvFLAGS(cv) & (CVf_CLONE|CVf_CLONED))
        return FALSE;
    if (CvOUTSIDE(cv) && !CvWEAKOUTSIDE(cv))
        return FALSE;
    return TRUE;
}

/*
=for apidoc mt_worker_tables_init

Allocates the per-worker depth and padlist tables for a boot image with
C<ncv> CVs.

=for apidoc mt_worker_tables_free

Releases the per-worker tables.  With C<svs_gone> false the SVs they refer
to are released too; with it true (after C<perl_destruct>, which frees
every SV of the worker) only the C-level memory is freed.

=cut
*/

void
Perl_mt_worker_tables_init(pTHX_ const struct perl_mt_image *img)
{
    U32 i;
    PERL_ARGS_ASSERT_MT_WORKER_TABLES_INIT;

    Perl_mt_worker_tables_free(aTHX_ FALSE);
    PL_mt_ncv = img->ncv;
    Newxz(PL_mt_cv_depth, PL_mt_ncv ? PL_mt_ncv : 1, I32);
    Newxz(PL_mt_padlists, PL_mt_ncv ? PL_mt_ncv : 1, PADLIST *);
    PL_mt_ngp = img->ngp;
    Newxz(PL_mt_gp_table, PL_mt_ngp ? PL_mt_ngp : 1, GP *);
    Newxz(PL_mt_gv_flags, PL_mt_ngp ? PL_mt_ngp : 1, STRLEN);
    for (i = 0; i < PL_mt_ngp; i++)
        PL_mt_gv_flags[i] = GvXPVGV(img->gvs[i])->xpv_cur;
    PL_mt_nhv = img->nhv;
    Newxz(PL_mt_hv_aux, PL_mt_nhv ? PL_mt_nhv : 1, struct xpvhv_aux *);
    Newxz(PL_mt_hv_shadow, PL_mt_nhv ? PL_mt_nhv : 1, HV *);
    Newxz(PL_mt_hv_amt, PL_mt_nhv ? PL_mt_nhv : 1, MAGIC *);
    Newxz(PL_mt_hv_amagic, PL_mt_nhv ? PL_mt_nhv : 1, U8);
    PL_eval_overlay_active = TRUE;
}

/*
=for apidoc mt_hv_amagic_nocontext

C<HvAMAGIC> of an immortal stash as seen by this interpreter: a worker
that has defined (or removed) overloads on the class since the freeze has
its own answer in C<PL_mt_hv_amagic>; otherwise the image's flag stands.

=for apidoc mt_hv_amagic_set_nocontext

C<HvAMAGIC_on> (C<state> 1) or C<HvAMAGIC_off> (C<state> 2) of an
immortal stash: recorded in this worker's table, since the stash head is
sealed.  Outside a worker (the main interpreter after the freeze, where
the stash is its own mutable copy anyway) nothing is recorded.

=cut
*/

bool
Perl_mt_hv_amagic_nocontext(const HV *hv)
{
    dTHX;
    PERL_ARGS_ASSERT_MT_HV_AMAGIC_NOCONTEXT;
    if (PL_mt_hv_amagic && HvHasAUX(hv)) {
        const U8 o = PL_mt_hv_amagic[HvMT_ID(HvAUX_raw(hv))];
        if (o)
            return o == 1;
    }
    return cBOOL(SvFLAGS(hv) & SVf_AMAGIC);
}

void
Perl_mt_hv_amagic_set_nocontext(HV *hv, U8 state)
{
    dTHX;
    PERL_ARGS_ASSERT_MT_HV_AMAGIC_SET_NOCONTEXT;
    if (PL_mt_hv_amagic && HvHasAUX(hv))
        PL_mt_hv_amagic[HvMT_ID(HvAUX_raw(hv))] = state;
}

/* one worker overload table (gv.c's AMT in a free-standing MAGIC) goes:
 * its callback releases the CVs it holds, then the memory */
static void
S_mt_amt_free(pTHX_ SV *stash, MAGIC **slot, bool svs_gone)
{
    MAGIC * const mg = *slot;
    if (!mg)
        return;
    *slot = NULL;
    if (!svs_gone && mg->mg_virtual && mg->mg_virtual->svt_free)
        mg->mg_virtual->svt_free(aTHX_ stash, mg);
    Safefree(mg->mg_ptr);
    Safefree(mg);
}

/*
=for apidoc mt_stash_amt

The overload table (C<PERL_MAGIC_overload_table>) of the immortal stash
C<stash> as seen by this worker: the worker's own if it has built one,
else the image's (read while it is current), else NULL.

=for apidoc mt_stash_amt_set

Stores a copy of the C<len>-byte overload table C<amt> as this worker's
table for the immortal stash C<stash>, replacing any earlier one.

=for apidoc mt_stash_amt_remove

Drops this worker's overload table for the immortal stash C<stash>.

=cut
*/

MAGIC *
Perl_mt_stash_amt(pTHX_ HV *stash)
{
    PERL_ARGS_ASSERT_MT_STASH_AMT;
    if (PL_mt_hv_amt && HvHasAUX(stash)) {
        MAGIC * const mg = PL_mt_hv_amt[HvMT_ID(HvAUX_raw(stash))];
        if (mg)
            return mg;
    }
    return mg_find((const SV *)stash, PERL_MAGIC_overload_table);
}

void
Perl_mt_stash_amt_set(pTHX_ HV *stash, const void *amt, STRLEN len)
{
    MAGIC *mg;
    MAGIC **slot;
    PERL_ARGS_ASSERT_MT_STASH_AMT_SET;

    if (!PL_mt_hv_amt || !HvHasAUX(stash))
        Perl_croak_mt(aTHX_ MT_IMMORTAL_MUTATION);
    slot = &PL_mt_hv_amt[HvMT_ID(HvAUX_raw(stash))];
    S_mt_amt_free(aTHX_ MUTABLE_SV(stash), slot, FALSE);
    Newxz(mg, 1, MAGIC);
    mg->mg_type = PERL_MAGIC_overload_table;
    mg->mg_virtual = (MGVTBL *)&PL_vtbl_ovrld;
    mg->mg_len = (I32)len;
    mg->mg_ptr = savepvn((const char *)amt, len);
    *slot = mg;
}

void
Perl_mt_stash_amt_remove(pTHX_ HV *stash)
{
    PERL_ARGS_ASSERT_MT_STASH_AMT_REMOVE;
    if (PL_mt_hv_amt && HvHasAUX(stash))
        S_mt_amt_free(aTHX_ MUTABLE_SV(stash),
                      &PL_mt_hv_amt[HvMT_ID(HvAUX_raw(stash))], FALSE);
}

/*
=for apidoc mt_worker_tables_release

Called by C<perl_destruct> in a worker just before C<sv_clean_all>: gives
up every SV the per-worker tables refer to (the worker GPs' values, the
shadow hashes, the mro copies, the padlists) and clears the entries, so
that nothing dangles while the arenas are swept; the memory of the tables
themselves goes with L</mt_worker_tables_free>.

=cut
*/

void
Perl_mt_worker_tables_release(pTHX)
{
    U32 i;
    if (PL_mt_hv_amt && PL_mt_image) {
        for (i = 0; i < PL_mt_nhv; i++)
            S_mt_amt_free(aTHX_ MUTABLE_SV(PL_mt_image->hvs[i]),
                          &PL_mt_hv_amt[i], FALSE);
    }
    if (PL_mt_padlists) {
        for (i = 0; i < PL_mt_ncv; i++)
            if (PL_mt_padlists[i]) {
                Perl_mt_padlist_free(aTHX_ PL_mt_padlists[i]);
                PL_mt_padlists[i] = NULL;
            }
    }
    if (PL_mt_hv_shadow) {
        for (i = 0; i < PL_mt_nhv; i++) {
            HV * const sh = PL_mt_hv_shadow[i];
            PL_mt_hv_shadow[i] = NULL;
            SvREFCNT_dec(sh);
        }
    }
    if (PL_mt_hv_aux) {
        for (i = 0; i < PL_mt_nhv; i++) {
            struct xpvhv_aux * const aux = PL_mt_hv_aux[i];
            struct mro_meta *meta;
            if (!aux || !(meta = aux->xhv_mro_meta))
                continue;
            if (meta->mro_linear_all) {
                SvREFCNT_dec(MUTABLE_SV(meta->mro_linear_all));
                meta->mro_linear_all = NULL;
            }
            else {
                SvREFCNT_dec(meta->mro_linear_current);
            }
            meta->mro_linear_current = NULL;
            SvREFCNT_dec(meta->mro_nextmethod); meta->mro_nextmethod = NULL;
            SvREFCNT_dec(meta->isa);            meta->isa = NULL;
            SvREFCNT_dec(meta->super);          meta->super = NULL;
            meta->destroy = NULL;
        }
    }
    if (PL_mt_gp_table) {
        for (i = 0; i < PL_mt_ngp; i++) {
            GP * const gp = PL_mt_gp_table[i];
            if (!gp)
                continue;
            SvREFCNT_dec(gp->gp_sv);   gp->gp_sv = NULL;
            SvREFCNT_dec(gp->gp_av);   gp->gp_av = NULL;
            SvREFCNT_dec(gp->gp_hv);   gp->gp_hv = NULL;
            SvREFCNT_dec(gp->gp_io);   gp->gp_io = NULL;
            SvREFCNT_dec(gp->gp_cv);   gp->gp_cv = NULL;
            SvREFCNT_dec(gp->gp_form); gp->gp_form = NULL;
            if (gp->gp_file_hek) {
                unshare_hek(gp->gp_file_hek);
                gp->gp_file_hek = NULL;
            }
        }
    }
}

void
Perl_mt_worker_tables_free(pTHX_ bool svs_gone)
{
    U32 i;
    if (PL_mt_hv_amt) {
        for (i = 0; i < PL_mt_nhv; i++)
            S_mt_amt_free(aTHX_ PL_mt_image ? MUTABLE_SV(PL_mt_image->hvs[i])
                                            : NULL,
                          &PL_mt_hv_amt[i], svs_gone);
        Safefree(PL_mt_hv_amt);
        PL_mt_hv_amt = NULL;
    }
    if (PL_mt_padlists) {
        for (i = 0; i < PL_mt_ncv; i++)
            if (PL_mt_padlists[i]) {
                if (svs_gone) {
                    Safefree(PadlistARRAY(PL_mt_padlists[i]));
                    Safefree(PL_mt_padlists[i]);
                }
                else
                    Perl_mt_padlist_free(aTHX_ PL_mt_padlists[i]);
            }
        Safefree(PL_mt_padlists);
        PL_mt_padlists = NULL;
    }
    Safefree(PL_mt_cv_depth);
    PL_mt_cv_depth = NULL;
    PL_mt_ncv = 0;
    Safefree(PL_mt_hv_amagic);
    PL_mt_hv_amagic = NULL;

    if (PL_mt_hv_shadow) {
        if (!svs_gone)
            for (i = 0; i < PL_mt_nhv; i++)
                SvREFCNT_dec(PL_mt_hv_shadow[i]);
        Safefree(PL_mt_hv_shadow);
        PL_mt_hv_shadow = NULL;
    }
    if (PL_mt_hv_aux) {
        for (i = 0; i < PL_mt_nhv; i++) {
            struct xpvhv_aux *aux = PL_mt_hv_aux[i];
            if (!aux)
                continue;
            if (aux->xhv_name_count)
                Safefree(aux->xhv_name_u.xhvnameu_names);
            if (aux->xhv_mro_meta) {
                struct mro_meta *meta = aux->xhv_mro_meta;
                if (!svs_gone) {
                    if (meta->mro_linear_all)
                        SvREFCNT_dec(MUTABLE_SV(meta->mro_linear_all));
                    else
                        SvREFCNT_dec(meta->mro_linear_current);
                    SvREFCNT_dec(meta->mro_nextmethod);
                    SvREFCNT_dec(meta->isa);
                    SvREFCNT_dec(meta->super);
                }
                Safefree(meta);
            }
            Safefree(aux);
        }
        Safefree(PL_mt_hv_aux);
        PL_mt_hv_aux = NULL;
    }
    PL_mt_nhv = 0;
    if (PL_mt_gp_table) {
        for (i = 0; i < PL_mt_ngp; i++)
            if (PL_mt_gp_table[i]) {
                GP * const gp = PL_mt_gp_table[i];
                /* a glob assignment (*a = *b) makes slots share a GP: the
                 * last reference frees it */
                if (gp->gp_refcnt > 1) {
                    gp->gp_refcnt--;
                    continue;
                }
                if (svs_gone)
                    Safefree(gp);
                else
                    Perl_mt_gp_release(aTHX_ gp);
            }
        Safefree(PL_mt_gp_table);
        PL_mt_gp_table = NULL;
    }
    Safefree(PL_mt_gv_flags);
    PL_mt_gv_flags = NULL;
    PL_mt_ngp = 0;
    PL_eval_overlay_active = FALSE;
}

/* ---- GP indirection -------------------------------------- */

/*
=for apidoc mt_gvgp

Returns this worker's GP for the immortal GV C<gv> (called through
C<GvGP>), materialising it from the template GP in the image on first
use.  Outside a worker the template itself is returned.

=for apidoc mt_gvgp_set

Implements C<GvGP_set> for an immortal GV: sets this worker's GP.

=for apidoc mt_gvflags_p

Returns the address of this worker's C<GvFLAGS> for the immortal GV.

=cut
*/

GP *
Perl_mt_gvgp(GV *gv)
{
    dTHX;
    GP *tpl;
    GP **slot;
    PERL_ARGS_ASSERT_MT_GVGP;

    tpl = GvGP_raw(gv);
    if (!tpl || !PL_mt_gp_table)
        return tpl;
    assert(tpl->gp_mt_id < PL_mt_ngp);
    slot = &PL_mt_gp_table[tpl->gp_mt_id];
    if (!*slot)
        *slot = Perl_mt_gp_materialize(aTHX_ gv);
    return *slot;
}

void
Perl_mt_gvgp_set(GV *gv, GP *gp)
{
    dTHX;
    GP *tpl;
    PERL_ARGS_ASSERT_MT_GVGP_SET;

    tpl = GvGP_raw(gv);
    if (!tpl || !PL_mt_gp_table) {
        if (PL_mt_gp_table)
            Perl_croak_mt(aTHX_ MT_IMMORTAL_MUTATION);
        GvGP_raw(gv) = gp;
        return;
    }
    PL_mt_gp_table[tpl->gp_mt_id] = gp;
}

STRLEN *
Perl_mt_gvflags_p(GV *gv)
{
    dTHX;
    GP *tpl;
    PERL_ARGS_ASSERT_MT_GVFLAGS_P;

    if (!PL_mt_gv_flags || !isGV_with_GP(gv) || !(tpl = GvGP_raw(gv)))
        return &GvXPVGV(gv)->xpv_cur;
    assert(tpl->gp_mt_id < PL_mt_ngp);
    return &PL_mt_gv_flags[tpl->gp_mt_id];
}

/*
=for apidoc mt_gp_materialize

Builds this worker's GP for the immortal GV C<gv>: the ithreads C<gp_dup>
logic applied to the template, with the worker's clone parameters, so the
scalar, array, hash and filehandle are private copies while the code and
format slots refer to the immortal CVs.

=for apidoc mt_gp_release

Frees a GP built by L</mt_gp_materialize>.

=cut
*/

GP *
Perl_mt_gp_materialize(pTHX_ GV *gv)
{
    GP *gp;
    PTR_TBL_t *saved;
    PERL_ARGS_ASSERT_MT_GP_MATERIALIZE;

    if (!PL_mt_clone_param)
        Perl_croak_mt(aTHX_ MT_NOT_A_WORKER);
    saved = PL_ptr_table;
    PL_ptr_table = PL_mt_ptr_table;
    gp = gp_dup(GvGP_raw(gv), PL_mt_clone_param);
    PL_ptr_table = saved;
    if (!gp->gp_refcnt)
        gp->gp_refcnt = 1;
    return gp;
}

void
Perl_mt_gp_release(pTHX_ GP *gp)
{
    PERL_ARGS_ASSERT_MT_GP_RELEASE;

    SvREFCNT_dec(gp->gp_sv);
    SvREFCNT_dec(gp->gp_av);
    SvREFCNT_dec(gp->gp_hv);
    SvREFCNT_dec(gp->gp_io);
    SvREFCNT_dec(gp->gp_cv);
    SvREFCNT_dec(gp->gp_form);
    if (gp->gp_file_hek)
        unshare_hek(gp->gp_file_hek);
    Safefree(gp);
}

/* ---- per-worker HV aux and the overlay --------------------- */

/*
=for apidoc mt_hv_aux

Returns this worker's copy of the aux structure of the immortal hash
C<hv> (called through C<HvAUX>): iterator state, names and mro meta are
copied from the image on first use so that iteration and method
resolution never write to it.  Outside a worker the image's own aux is
returned.

=cut
*/

struct xpvhv_aux *
Perl_mt_hv_aux_nocontext(HV *hv)
{
    dTHX;
    PERL_ARGS_ASSERT_MT_HV_AUX_NOCONTEXT;
    return Perl_mt_hv_aux(aTHX_ hv);
}

struct xpvhv_aux *
Perl_mt_hv_aux(pTHX_ HV *hv)
{
    struct xpvhv_aux *raw;
    struct xpvhv_aux *aux;
    U32 id;
    PERL_ARGS_ASSERT_MT_HV_AUX;

    raw = HvAUX_raw(hv);
    if (!PL_mt_hv_aux)
        return raw;
    id = HvMT_ID(raw);
    assert(id < PL_mt_nhv);
    aux = PL_mt_hv_aux[id];
    if (aux)
        return aux;

    Newx(aux, 1, struct xpvhv_aux);
    Copy(raw, aux, 1, struct xpvhv_aux);
    aux->xhv_backreferences = NULL;
    aux->xhv_eiter = NULL;
    aux->xhv_riter = -1;
#ifdef PERL_HASH_RANDOMIZE_KEYS
    aux->xhv_last_rand = aux->xhv_rand;
#endif
    if (aux->xhv_name_count) {
        const I32 n = aux->xhv_name_count < 0
                        ? -aux->xhv_name_count : aux->xhv_name_count;
        HEK **names;
        Newx(names, n, HEK *);
        Copy(raw->xhv_name_u.xhvnameu_names, names, n, HEK *);
        aux->xhv_name_u.xhvnameu_names = names;
    }
    if (raw->xhv_mro_meta) {
        PTR_TBL_t *saved = PL_ptr_table;
        if (!PL_mt_clone_param)
            Perl_croak_mt(aTHX_ MT_NOT_A_WORKER);
        PL_ptr_table = PL_mt_ptr_table;
        aux->xhv_mro_meta = mro_meta_dup(raw->xhv_mro_meta, PL_mt_clone_param);
        PL_ptr_table = saved;
    }
    PL_mt_hv_aux[id] = aux;
    return aux;
}

/*
=for apidoc mt_hv_common_immortal

The C<hv_common> of an immortal hash: reads consult the image first and
then this worker's shadow of the hash; stores and deletes of keys that
are not in the image go to the shadow, which is created on demand (this
is how C<eval>, C<require> and method caches add to frozen stashes);
changing or deleting a key that is in the image raises
C<MT_IMMORTAL_MUTATION>.  Outside a worker the image is read directly.

=cut
*/

void *
Perl_mt_hv_common_immortal(pTHX_ HV *hv, SV *keysv, const char *key,
                           STRLEN klen, int flags, int action, SV *val,
                           U32 hash)
{
    const int raw_flags = flags & ~HVhek_FREEKEY;
    HV *shadow;
    U32 id;
    void *r;
    PERL_ARGS_ASSERT_MT_HV_COMMON_IMMORTAL;

    if (!PL_mt_hv_shadow || !HvHasAUX(hv)) {
        /* not a worker (or a frozen hash with no aux): the image itself */
        if (action & (HV_DELETE|HV_FETCH_ISSTORE))
            Perl_croak_mt(aTHX_ MT_IMMORTAL_MUTATION);
        return Perl_hv_common(aTHX_ hv, keysv, key, klen, flags,
                              action | HV_MT_RAW, val, hash);
    }
    id = HvMT_ID(HvAUX_raw(hv));
    assert(id < PL_mt_nhv);
    shadow = PL_mt_hv_shadow[id];

    /* The shadow takes precedence over the image (a key stored again in
     * the worker, e.g. a sub redefined or a stash slot upgraded to a
     * glob, is seen instead of the frozen one); stores always go to the
     * shadow; an lvalue fetch of a key that is in the image yields the
     * image's entry.  The key is freed here when the caller asked for it,
     * never by the raw calls. */
    if (action & HV_DELETE) {
        if (shadow
            && Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags,
                              HV_FETCH_ISEXISTS, NULL, hash)) {
            r = Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags,
                               action, val, hash);
        }
        else if (Perl_hv_common(aTHX_ hv, keysv, key, klen, raw_flags,
                                HV_FETCH_ISEXISTS | HV_MT_RAW, NULL, hash)) {
            /* a frozen key cannot go; global destruction (perl_destruct
             * clearing main::) simply leaves it there */
            if (PL_phase != PERL_PHASE_DESTRUCT) {
                if (flags & HVhek_FREEKEY)
                    Safefree(key);
                Perl_croak_mt(aTHX_ MT_IMMORTAL_MUTATION);
            }
            r = NULL;
        }
        else
            r = NULL;
    }
    else if (action & HV_FETCH_ISSTORE) {
        if (!shadow)
            shadow = PL_mt_hv_shadow[id] = newHV();
        r = Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags, action,
                           val, hash);
    }
    else if (action & HV_FETCH_LVALUE) {
        const int look = (action & ~HV_FETCH_LVALUE) | HV_FETCH_JUST_SV;
        r = shadow ? Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags,
                                    (look & ~HV_FETCH_JUST_SV)
                                        | (action & HV_FETCH_JUST_SV),
                                    NULL, hash)
                   : NULL;
        if (!r)
            r = Perl_hv_common(aTHX_ hv, keysv, key, klen, raw_flags,
                               (action & ~HV_FETCH_LVALUE) | HV_MT_RAW,
                               NULL, hash);
        if (!r) {
            if (!shadow)
                shadow = PL_mt_hv_shadow[id] = newHV();
            r = Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags,
                               action, val, hash);
        }
    }
    else {
        /* fetch or exists */
        r = shadow ? Perl_hv_common(aTHX_ shadow, keysv, key, klen, raw_flags,
                                    action, val, hash)
                   : NULL;
        if (!r)
            r = Perl_hv_common(aTHX_ hv, keysv, key, klen, raw_flags,
                               action | HV_MT_RAW, val, hash);
    }
    if (flags & HVhek_FREEKEY)
        Safefree(key);
    return r;
}

/*
=for apidoc mt_gv_init_private

C<gv_init_pvn> for a stash slot that may belong to the boot image: an
ordinary slot is upgraded in place and returned; an immortal one (a
frozen sub stored without a glob, or a prototype stub) is copied, the
copy is made the glob and stored in this worker's shadow of the stash,
where it takes precedence over the frozen slot, and the copy is
returned.

=cut
*/

GV *
Perl_mt_gv_init_private(pTHX_ HV *stash, SV *slot, const char *name,
                        STRLEN len, U32 flags)
{
    GV *gv;
    PERL_ARGS_ASSERT_MT_GV_INIT_PRIVATE;

    if (!(SvFLAGS(slot) & SVf_IMMORTAL)) {
        gv_init_pvn((GV *)slot, stash, name, len, flags);
        return (GV *)slot;
    }
    if (!PL_mt_hv_shadow)
        Perl_croak_mt(aTHX_ MT_IMMORTAL_MUTATION);
    gv = (GV *)newSVsv(slot);
    gv_init_pvn(gv, stash, name, len, flags);
    (void)hv_common_key_len(stash, name,
                            (flags & SVf_UTF8) ? -(I32)len : (I32)len,
                            HV_FETCH_ISSTORE, (SV *)gv, 0);
    return gv;
}

/* ---- regexps ------------------------------------------------ */

/*
=for apidoc mt_regex_materialize

Replaces the immortal regexp of the match op C<o> in this worker's regex
pad by a private copy (C<re_dup_guts>: the head holds match state) and
returns it.

=cut
*/

REGEXP *
Perl_mt_regex_materialize(pTHX_ PMOP *o)
{
    SV *src;
    SV *dst;
    PTR_TBL_t *saved;
    PERL_ARGS_ASSERT_MT_REGEX_MATERIALIZE;

    src = PL_regex_pad[o->op_pmoffset];
    if (!PL_mt_clone_param)
        Perl_croak_mt(aTHX_ MT_NOT_A_WORKER);
    saved = PL_ptr_table;
    PL_ptr_table = PL_mt_ptr_table;
    PL_mt_clone_param->flags |= CLONEf_MT_COPY_RE;
    dst = sv_dup_inc(src, PL_mt_clone_param);
    PL_mt_clone_param->flags &= ~CLONEf_MT_COPY_RE;
    PL_ptr_table = saved;
    PL_regex_pad[o->op_pmoffset] = dst;   /* the immortal is not counted */
    return (REGEXP *)dst;
}

/*
=for apidoc mt_sv_is_shared_kind

Whether an immortal SV of this kind is shared with a worker as it is
(code, formats, globs, named stashes, regexps) rather than copied into it.

=cut
*/

bool
Perl_mt_sv_is_shared_kind(const SV *sv)
{
    PERL_ARGS_ASSERT_MT_SV_IS_SHARED_KIND;

    switch (SvTYPE(sv)) {
    case SVt_PVCV:
    case SVt_PVFM:
    case SVt_PVGV:
    case SVt_REGEXP:
        return TRUE;
    case SVt_PVLV:
        return isGV_with_GP(sv);
    case SVt_PVHV:
        if (!HvHasAUX(sv))
            return FALSE;
        {
            const struct xpvhv_aux *aux = HvAUX_raw(sv);
            return aux->xhv_name_count
                     ? aux->xhv_name_u.xhvnameu_names[0] != NULL
                     : aux->xhv_name_u.xhvnameu_name != NULL;
        }
    default:
        return FALSE;
    }
}

/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

/* ---- worker interpreters ------------------------------------- */

/*
 * A worker is a fresh interpreter cloned from the boot image with
 * CLONEf_MT_WORKER: code, globs, stashes and regexps are shared with the
 * image, every other SV the interpreter struct refers to is copied, and
 * what the shared GVs, CVs and stashes would be written through goes to
 * the per-worker tables above.  The worker runs one named sub of the
 * image with marshalled scalar arguments and returns one scalar (or its
 * error) to whoever joins it.  Workers are pthreads; the registry below is
 * process-wide.
 */

#include "XSUB.h"

typedef struct perl_mt_worker {
    U32                  id;
    pthread_t            tid;
    int                  state;     /* 0 running, 1 finished, 2 joined   */
    bool                 joinable;
    char                *subname;   /* PerlMemShared                      */
    STRLEN               sublen;
    perl_mt_value_t     *args;
    U32                  nargs;
    perl_mt_value_t      result;
    char                *error;     /* PerlMemShared, NULL when none      */
    int                  exited;    /* exit() called: the code            */
    int                  faulted_sig; /* caught fatal signal              */
} perl_mt_worker_t;

static perl_mutex          S_mt_workers_mutex;
static int                 S_mt_workers_inited = 0;
static perl_mt_worker_t  **S_mt_workers = NULL;
static U32                 S_mt_nworkers = 0, S_mt_workers_cap = 0;
static U32                 S_mt_next_id = 1;
static U32                 S_mt_live_workers = 0;

/*
=for apidoc mt_value_from_sv

Marshals the plain scalar C<sv> (undef, integer, number, string) or a
capability handle into C<v>, memory that any thread may read; a
reference to anything else is refused with C<MT_UNSHAREABLE_REF>.  A
capability handle is counted as shared (the receiver owns one handle).

=for apidoc mt_value_to_sv

A new SV from a marshalled value.

=for apidoc mt_value_free

Releases the memory a marshalled value owns.

=cut
*/

void
Perl_mt_value_from_sv(pTHX_ perl_mt_value_t *v, SV *sv)
{
    PERL_ARGS_ASSERT_MT_VALUE_FROM_SV;
    Zero(v, 1, perl_mt_value_t);
    if (!sv || !SvOK(sv)) {
        v->kind = 'u';
    }
    else if (Perl_mt_cap_is_handle(aTHX_ sv)) {
        U32 slot, gen;
        Perl_mt_cap_share(aTHX_ sv, &slot, &gen);
        v->kind = 'c';
        v->iv = slot;
        v->gen = gen;
    }
    else if (SvROK(sv)) {
        Perl_croak_mt(aTHX_ MT_UNSHAREABLE_REF);
    }
    else if (SvIOK(sv) && !SvPOK(sv)) {
        v->kind = 'i';
        v->iv = SvIV(sv);
    }
    else if (SvNOK(sv) && !SvPOK(sv)) {
        v->kind = 'n';
        v->nv = SvNV(sv);
    }
    else {
        STRLEN len;
        const char *p = SvPV(sv, len);
        v->kind = 'p';
        v->utf8 = cBOOL(SvUTF8(sv));
        v->pv = (char *)PerlMemShared_malloc(len + 1);
        Copy(p, v->pv, len, char);
        v->pv[len] = '\0';
        v->len = len;
    }
}

SV *
Perl_mt_value_to_sv(pTHX_ const perl_mt_value_t *v)
{
    PERL_ARGS_ASSERT_MT_VALUE_TO_SV;
    switch (v->kind) {
    case 'c': return Perl_mt_cap_handle_new(aTHX_ (U32)v->iv, v->gen);
    case 'i': return newSViv(v->iv);
    case 'n': return newSVnv(v->nv);
    case 'p': {
        SV * const sv = newSVpvn(v->pv, v->len);
        if (v->utf8)
            SvUTF8_on(sv);
        return sv;
    }
    default:  return newSV_type(SVt_NULL);
    }
}

void
Perl_mt_value_free(perl_mt_value_t *v)
{
    PERL_ARGS_ASSERT_MT_VALUE_FREE;
    if (v->kind == 'p' && v->pv)
        PerlMemShared_free(v->pv);
    v->pv = NULL;
}

static void
S_mt_worker_free(perl_mt_worker_t *w)
{
    U32 i;
    if (w->args) {
        for (i = 0; i < w->nargs; i++)
            Perl_mt_value_free(&w->args[i]);
        PerlMemShared_free(w->args);
    }
    Perl_mt_value_free(&w->result);
    if (w->error)
        PerlMemShared_free(w->error);
    if (w->subname)
        PerlMemShared_free(w->subname);
    PerlMemShared_free(w);
}

static char *
S_mt_shared_strdup(const char *s, STRLEN len)
{
    char * const p = (char *)PerlMemShared_malloc(len + 1);
    Copy(s, p, len, char);
    p[len] = '\0';
    return p;
}

/* The body of a worker thread: build the interpreter on the image, run the
 * sub, record the result, tear the interpreter down. */
/* The CLONE methods of the image's classes, called in the new worker as
 * perl_clone calls them in a new thread (perlmod): XS modules re-create
 * their per-interpreter state (MY_CXT_CLONE) here, so that it is never the
 * image's read-only copy that they write to. */
static void
S_mt_worker_clone_methods(pTHX_ const struct perl_mt_image *img)
{
    AV * const stashes = img->clone_stashes;
    SSize_t i, n;

    if (!stashes)
        return;
    n = AvFILLp(stashes) + 1;
    for (i = 0; i < n; i++) {
        HV * const stash = MUTABLE_HV(AvARRAY(stashes)[i]);
        GV * const cloner = gv_fetchmethod_autoload(stash, "CLONE", 0);
        if (cloner && GvCV(cloner)) {
            SV *newsv;
            ENTER;
            SAVETMPS;
            PUSHMARK(PL_stack_sp);
            rpp_extend(1);
            newsv = newSVhek(HvNAME_HEK(stash));
            *++PL_stack_sp = newsv;
            if (!rpp_stack_is_rc())
                sv_2mortal(newsv);
            call_sv(MUTABLE_SV(GvCV(cloner)), G_DISCARD);
            FREETMPS;
            LEAVE;
        }
    }
}

static int
S_mt_worker_runops(pTHX)
{
    OP *op = PL_op;
    U32 ops = 0;
    PERL_DTRACE_PROBE_OP(op);
    while ((PL_op = op = op->op_ppaddr(aTHX))) {
        PERL_DTRACE_PROBE_OP(op);
        if (UNLIKELY((++ops & 0x3fffu) == 0))
            Perl_mt_sup_maybe_beat();
    }
    PERL_ASYNC_CHECK();
    TAINT_NOT;
    return 0;
}

static void *
S_mt_worker_main(void *arg)
{
    perl_mt_worker_t * const w = (perl_mt_worker_t *)arg;
    const struct perl_mt_image * const img = PL_mt_image;
    PerlInterpreter *my_perl;
    CV *cv = NULL;
    int jmp_rc;
    int32_t sup_slot;
    perl_mt_worker_sig_ctx_t *sig_ctx;
    JMPENV *saved_top_env;
    volatile int sig_caught = 0;
    dJMPENV;

    /* install worker fault signal handlers and per-thread altstack */
    Perl_mt_fault_install_signal_handlers();
    sig_ctx = Perl_mt_fault_worker_sig_ctx();
    (void)Perl_mt_fault_altstack_setup(sig_ctx);
    sig_ctx->worker_id = w->id;

    /* the image is readable and unwritable in this thread too */
    if (PL_mt_image_key_armed)
        (void)perl_krt_key_arm(PL_mt_image_key, PERL_KRT_KEY_NOWRITE);

    my_perl = perl_clone(img->interp, CLONEf_KEEP_PTR_TABLE | CLONEf_MT_WORKER);
    /* perl_clone has made the new interpreter the current one */
    PL_mt_ptr_table = PL_ptr_table;
    PL_ptr_table = NULL;
    PL_mt_clone_param = Perl_clone_params_new(img->interp, my_perl);
    PL_mt_clone_param->flags |= CLONEf_MT_WORKER;
    Perl_mt_worker_tables_init(aTHX_ img);
    thread_locale_init();
    PL_mt_worker_id = w->id;
    Perl_mt_fiber_init(aTHX);       /* fibers never cross into a worker */
    Perl_mt_cap_worker_start(aTHX);
    sup_slot = Perl_mt_sup_register(MT_UNIT_WORKER, w->id, 1u);
    Perl_mt_sup_set_tls_slot(sup_slot);
    if (PL_runops == Perl_runops_standard)
        PL_runops = S_mt_worker_runops;

    /* the main program's pad is this worker's own copy (fresh lexicals) */
    PL_comppad = PadlistARRAY(CvPADLIST(PL_main_cv))[1];
    PL_curpad = AvARRAY(PL_comppad);
    PL_endav = NULL;                 /* END blocks belong to the parent */
    /* full SV cleanup, but not the string-table balance check: a few
     * file-name heks copied into the worker are over-counted (the table is
     * freed as a whole either way) */
    PL_perl_destruct_level = 1;
    PERL_SET_PHASE(PERL_PHASE_RUN);

    saved_top_env = PL_top_env;
    sig_caught = sigsetjmp(sig_ctx->jmp, 1);
    if (sig_caught == 0) {
        sig_ctx->caught_sig = 0;
        sig_ctx->armed = 1;

        JMPENV_PUSH(jmp_rc);
        if (jmp_rc == 0) {
            GV *gv;
            U32 i;

            ENTER;
            SAVETMPS;

            S_mt_worker_clone_methods(aTHX_ img);
            gv = gv_fetchpvn_flags(w->subname, w->sublen, 0, SVt_PVCV);
            cv = gv && isGV(gv) ? GvCV(gv) : NULL;
            if (!cv) {
                w->error = S_mt_shared_strdup("perl5-mt: worker entry sub not "
                                              "found (MT_WORKER_FAILED)", 54);
                Perl_mt_fault_record(MT_FAULT_ERR_CROAK, MT_FAULT_RECOVERABLE,
                                     MT_UNIT_WORKER, w->id, 1u, MT_WORKER_FAILED,
                                     w->error);
            }
            else {
                SV *res;
                PUSHMARK(PL_stack_sp);
                for (i = 0; i < w->nargs; i++) {
                    SV * const sv = sv_2mortal(Perl_mt_value_to_sv(aTHX_ &w->args[i]));
                    rpp_extend(1);
                    *++PL_stack_sp = sv;
                }
                call_sv(MUTABLE_SV(cv), G_SCALAR | G_EVAL);
                res = *PL_stack_sp;
                if (SvTRUE(ERRSV)) {
                    STRLEN len;
                    const char * const e = SvPV(ERRSV, len);
                    w->error = S_mt_shared_strdup(e, len);
                    /* Do not duplicate a fault already recorded by Perl_mt_fault_raise */
                    if (len < 17 || memcmp(e, "perl5-mt: fault #", 17) != 0) {
                        Perl_mt_fault_record(MT_FAULT_ERR_CROAK, MT_FAULT_RECOVERABLE,
                                             MT_UNIT_WORKER, w->id, 1u, MT_WORKER_FAILED,
                                             w->error);
                    }
                }
                else {
                    Perl_mt_value_from_sv(aTHX_ &w->result, res);
                }
                PL_stack_sp--;
            }
            FREETMPS;
            LEAVE;
        }
        else if (jmp_rc == 2) {
            /* exit() inside the worker */
            w->exited = 1;
            w->error = S_mt_shared_strdup("perl5-mt: exit() called in a worker "
                                          "(MT_WORKER_FAILED)", 54);
            Perl_mt_fault_record(MT_FAULT_WORKER_EXIT, MT_FAULT_RECOVERABLE,
                                 MT_UNIT_WORKER, w->id, 1u, MT_WORKER_FAILED,
                                 w->error);
        }
        else {
            w->error = S_mt_shared_strdup("perl5-mt: worker died outside eval "
                                          "(MT_WORKER_FAILED)", 54);
            Perl_mt_fault_record(MT_FAULT_ERR_CROAK, MT_FAULT_RECOVERABLE,
                                 MT_UNIT_WORKER, w->id, 1u, MT_WORKER_FAILED,
                                 w->error);
        }
        JMPENV_POP;
        sig_ctx->armed = 0;
    }
    else {
        /* worker caught SIGSEGV/SIGBUS/SIGFPE and unwound safely.
         * On Linux x86_64 with Intel MPK (PKRU), signal delivery resets PKRU
         * to init_pkru (blocking even reads on allocated pkeys), and
         * siglongjmp bypasses sigreturn; re-arm NOWRITE so perl_destruct can
         * read frozen-image SVs safely. */
        char msg[MT_FAULT_MSG_MAX];
        int sig = (int)sig_ctx->caught_sig;
        sig_ctx->armed = 0;
        if (PL_mt_image_key_armed)
            (void)perl_krt_key_arm(PL_mt_image_key, PERL_KRT_KEY_NOWRITE);
        PL_top_env = saved_top_env;
        (void)snprintf(msg, sizeof msg,
                       "perl5-mt: worker %u caught %s at %p (MT_WORKER_FAULTED)",
                       (unsigned)w->id,
                       Perl_mt_fault_signal_name(sig),
                       (void *)sig_ctx->fault_addr);
        w->faulted_sig = sig;
        w->error = S_mt_shared_strdup(msg, strlen(msg));
        Perl_mt_fault_record(MT_FAULT_SIGNAL, MT_FAULT_RECOVERABLE,
                             MT_UNIT_WORKER, w->id, 1u, MT_WORKER_FAILED,
                             msg);
    }

    Perl_mt_sup_unregister(sup_slot);
    Perl_mt_sup_set_tls_slot(-1);

    PerlIO_flush((PerlIO *)NULL);
    thread_locale_term();

    /* tear down: perl_destruct frees every SV of the worker (calling
     * Perl_mt_worker_tables_release along the way), then the tables' own
     * memory goes; the shared op tree and the image are left alone. */
    {
        CLONE_PARAMS * const param = PL_mt_clone_param;
        SvREFCNT_dec(param->stashes);
        SvREFCNT_dec(param->unreferenced);   /* drops this thread's refs */
        param->stashes = newAV();
        AvREAL_off(param->stashes);
        param->unreferenced = newAV();
    }
    /* the XS bridge slot goes back to the pool, with the host code refs
     * it pinned for compartments (SVs: before the sweep) */
    Perl_mt_xs_slot_release(aTHX);
    PL_main_root = NULL;
    PL_main_start = NULL;
    perl_destruct(my_perl);
    /* the capability sweep: handles went with the SVs; the inbox is
     * drained and future notifications go to the main interpreter */
    Perl_mt_cap_worker_exit(aTHX);
    Perl_mt_worker_tables_free(aTHX_ TRUE);
    Safefree(PL_mt_clone_param);
    PL_mt_clone_param = NULL;
    if (PL_mt_ptr_table) {
        ptr_table_free(PL_mt_ptr_table);
        PL_mt_ptr_table = NULL;
    }
    perl_free(my_perl);

    Perl_mt_fault_altstack_teardown(sig_ctx);

    MUTEX_LOCK(&S_mt_workers_mutex);
    w->state = 1;
    S_mt_live_workers--;
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    return NULL;
}

/*
=for apidoc mt_worker_spawn

Starts a worker interpreter on the boot image running the named sub
C<name> (C<"main::work">, C<"Foo::bar">) with the scalar arguments
C<args>; references are refused with C<MT_UNSHAREABLE_REF>.  Returns the
worker id.  Dies with C<MT_NOT_FROZEN> before the freeze.

=for apidoc mt_worker_join

Waits for the worker and returns its result (a new SV, undef when the sub
returned nothing); dies with the worker's error when it died.

=cut
*/

/* The stack of a worker thread.  glibc gives a secondary thread 8 MiB, but
 * macOS gives it 512 KiB, which is why the remote-XS depth cap is 512.  Every
 * platform now gets this size: MT_WORKER_STACK (bytes) overrides it, and a
 * value below PTHREAD_STACK_MIN or not a number is ignored.  The main thread's
 * stack is not touched. */
#define MT_WORKER_STACK_DEFAULT (8 * 1024 * 1024)

static size_t
S_mt_worker_stack_size(void)
{
    const char *env = getenv("MT_WORKER_STACK");
    size_t want = MT_WORKER_STACK_DEFAULT;
    if (env && *env) {
        char *end;
        unsigned long v = strtoul(env, &end, 10);
        if (*end == '\0' && v >= (unsigned long)PTHREAD_STACK_MIN)
            want = (size_t)v;
    }
    /* pthread_attr_setstacksize wants whole pages on macOS */
    {
        const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
        want = (want + pg - 1) / pg * pg;
    }
    return want;
}

static int
S_mt_worker_create(pthread_t *tid, perl_mt_worker_t *w)
{
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0)
        return rc;
    rc = pthread_attr_setstacksize(&attr, S_mt_worker_stack_size());
    if (rc == 0)
        rc = pthread_create(tid, &attr, S_mt_worker_main, w);
    pthread_attr_destroy(&attr);
    return rc;
}

U32
Perl_mt_worker_spawn(pTHX_ SV *name, SV **args, U32 nargs)
{
    perl_mt_worker_t *w;
    STRLEN len;
    const char *pv;
    U32 i;
    int rc;

    PERL_ARGS_ASSERT_MT_WORKER_SPAWN;

    if (!PL_mt_image)
        Perl_croak_mt(aTHX_ MT_NOT_FROZEN);
    if (!S_mt_workers_inited)
        Perl_croak_mt(aTHX_ MT_WORKER_FAILED);

    w = (perl_mt_worker_t *)PerlMemShared_calloc(1, sizeof *w);
    pv = SvPV(name, len);
    if (len < 2 || !memchr(pv, ':', len)) {
        /* unqualified: main:: */
        w->subname = (char *)PerlMemShared_malloc(len + 7);
        Copy("main::", w->subname, 6, char);
        Copy(pv, w->subname + 6, len, char);
        w->subname[len + 6] = '\0';
        w->sublen = len + 6;
    }
    else {
        w->subname = S_mt_shared_strdup(pv, len);
        w->sublen = len;
    }
    w->nargs = nargs;
    if (nargs) {
        w->args = (perl_mt_value_t *)PerlMemShared_calloc(nargs, sizeof *w->args);
        for (i = 0; i < nargs; i++)
            Perl_mt_value_from_sv(aTHX_ &w->args[i], args[i]);
    }
    w->result.kind = 'u';

    MUTEX_LOCK(&S_mt_workers_mutex);
    w->id = S_mt_next_id++;
    if (S_mt_nworkers == S_mt_workers_cap) {
        S_mt_workers_cap = S_mt_workers_cap ? S_mt_workers_cap * 2 : 16;
        S_mt_workers = (perl_mt_worker_t **)
            PerlMemShared_realloc(S_mt_workers,
                                  S_mt_workers_cap * sizeof *S_mt_workers);
    }
    S_mt_workers[S_mt_nworkers++] = w;
    S_mt_live_workers++;
    w->joinable = TRUE;
    rc = S_mt_worker_create(&w->tid, w);
    if (rc != 0) {
        S_mt_nworkers--;
        S_mt_live_workers--;
        MUTEX_UNLOCK(&S_mt_workers_mutex);
        S_mt_worker_free(w);
        Perl_croak_mt(aTHX_ MT_WORKER_FAILED);
    }
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    return w->id;
}

static perl_mt_worker_t *
S_mt_worker_find(U32 id, U32 *idx)
{
    U32 i;
    for (i = 0; i < S_mt_nworkers; i++)
        if (S_mt_workers[i]->id == id) {
            if (idx) *idx = i;
            return S_mt_workers[i];
        }
    return NULL;
}

SV *
Perl_mt_worker_join(pTHX_ U32 id)
{
    perl_mt_worker_t *w;
    U32 idx = 0;
    SV *res;
    char *error = NULL;

    MUTEX_LOCK(&S_mt_workers_mutex);
    w = S_mt_worker_find(id, &idx);
    if (!w || !w->joinable) {
        MUTEX_UNLOCK(&S_mt_workers_mutex);
        Perl_croak(aTHX_ "perl5-mt: no joinable worker %" UVuf
                         " (MT_WORKER_FAILED)", (UV)id);
    }
    w->joinable = FALSE;
    MUTEX_UNLOCK(&S_mt_workers_mutex);

    (void)pthread_join(w->tid, NULL);

    MUTEX_LOCK(&S_mt_workers_mutex);
    w->state = 2;
    res = w->error ? NULL : Perl_mt_value_to_sv(aTHX_ &w->result);
    if (w->error)
        error = w->error, w->error = NULL;
    /* drop it from the registry */
    if (S_mt_worker_find(id, &idx)) {
        S_mt_workers[idx] = S_mt_workers[--S_mt_nworkers];
    }
    S_mt_worker_free(w);
    MUTEX_UNLOCK(&S_mt_workers_mutex);

    if (error) {
        SV * const errsv = newSVpv(error, 0);
        PerlMemShared_free(error);
        sv_2mortal(errsv);
        Perl_croak_sv(aTHX_ errsv);
    }
    return res;
}

/*
=for apidoc mt_worker_join_all

Called when the main interpreter is destroyed.  The workers that still run
are joined first: their interpreters use the process's mutexes and tables,
which the destruction is about to free.  A warning names their number.

=cut
*/

void
Perl_mt_worker_join_all(pTHX)
{
    U32 i, n = 0;
    perl_mt_worker_t **todo;
    if (PL_mt_worker_id)
        return;                     /* a worker's own destruction */
    if (!S_mt_workers_inited)
        return;                     /* no worker was ever started: no mutex */
    MUTEX_LOCK(&S_mt_workers_mutex);
    Newx(todo, S_mt_nworkers + 1, perl_mt_worker_t *);
    for (i = 0; i < S_mt_nworkers; i++) {
        if (S_mt_workers[i]->joinable) {
            S_mt_workers[i]->joinable = FALSE;
            todo[n++] = S_mt_workers[i];
        }
    }
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    if (n) {
        Perl_warn(aTHX_ "perl5-mt: %d worker(s) still running at exit; "
                        "joined them before the interpreter was destroyed",
                  (int)n);
    }
    for (i = 0; i < n; i++) {
        perl_mt_worker_t * const w = todo[i];
        U32 idx = 0;
        (void)pthread_join(w->tid, NULL);
        MUTEX_LOCK(&S_mt_workers_mutex);
        w->state = 2;
        if (S_mt_worker_find(w->id, &idx)) {
            S_mt_workers[idx] = S_mt_workers[--S_mt_nworkers];
        }
        S_mt_worker_free(w);
        MUTEX_UNLOCK(&S_mt_workers_mutex);
    }
    Safefree(todo);
}

/*
=for apidoc perl_mt_active_workers

The number of worker interpreters alive in the process (the main
interpreter is not one).  The XS dispatcher consults it: a module without
the threadsafe marker is loaded into a compartment while this is non-zero.

=cut
*/

U32
perl_mt_active_workers(pTHX)
{
    U32 n;
    PERL_UNUSED_CONTEXT;
    if (!S_mt_workers_inited)
        return 0;
    MUTEX_LOCK(&S_mt_workers_mutex);
    n = S_mt_live_workers;
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    return n;
}

/* Non-blocking worker state query for MT::Pool supervision:
 *   0 = running
 *   1 = finished cleanly (ready to join)
 *  -1 = finished with error or fault (ready to join)
 *  -2 = unknown or already joined */
int
Perl_mt_worker_state(uint32_t id)
{
    perl_mt_worker_t *w;
    int rc = -2;
    if (!S_mt_workers_inited)
        return -2;
    MUTEX_LOCK(&S_mt_workers_mutex);
    w = S_mt_worker_find((U32)id, NULL);
    if (w) {
        if (w->state == 0)
            rc = 0;
        else if (w->state == 1)
            rc = w->error ? -1 : 1;
    }
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    return rc;
}

int
Perl_mt_worker_faulted_sig(uint32_t id)
{
    perl_mt_worker_t *w;
    int sig = 0;
    if (!S_mt_workers_inited)
        return 0;
    MUTEX_LOCK(&S_mt_workers_mutex);
    w = S_mt_worker_find((U32)id, NULL);
    if (w)
        sig = w->faulted_sig;
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    return sig;
}

/* ---- MT::Worker --------------------------------------------------------- */

XS(XS_MT_Worker_spawn);
XS(XS_MT_Worker_spawn)
{
    dXSARGS;
    U32 id;
    if (items < 1)
        croak_xs_usage(cv, "subname, args...");
    id = Perl_mt_worker_spawn(aTHX_ ST(0), items > 1 ? &ST(1) : NULL,
                              (U32)(items - 1));
    ST(0) = sv_2mortal(newSVuv(id));
    XSRETURN(1);
}

XS(XS_MT_Worker_join);
XS(XS_MT_Worker_join)
{
    dXSARGS;
    SV *res;
    if (items != 1)
        croak_xs_usage(cv, "id");
    res = Perl_mt_worker_join(aTHX_ (U32)SvUV(ST(0)));
    ST(0) = sv_2mortal(res);
    XSRETURN(1);
}

XS(XS_MT_Worker_count);
XS(XS_MT_Worker_count)
{
    dXSARGS;
    U32 n;
    PERL_UNUSED_VAR(items);
    MUTEX_LOCK(&S_mt_workers_mutex);
    n = S_mt_live_workers;
    MUTEX_UNLOCK(&S_mt_workers_mutex);
    ST(0) = sv_2mortal(newSVuv(n));
    XSRETURN(1);
}

XS(XS_MT_Worker_is_worker);
XS(XS_MT_Worker_is_worker)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = PL_eval_overlay_active ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

/* ---- MT::Ctx: the fiber context switch ---------------------------------- */

/* Whether the kernel has the x86 shadow stack enabled for this process
 * (ARCH_SHSTK_STATUS).  Zero everywhere else, and where the kernel does not
 * answer the query. */
int
Perl_mt_ctx_shstk_active(void)
{
#if defined(__x86_64__) && defined(__linux__)
    unsigned long status = 0;
    if (syscall(SYS_arch_prctl, 0x5005 /* ARCH_SHSTK_STATUS */, &status) != 0)
        return 0;
    return (int)(status & 1);
#else
    return 0;
#endif
}

#define MT_CTX_SELFTEST_STACK (64 * 1024)
#define MT_CTX_SELFTEST_KEY   0x9e3779b97f4a7c15UL

struct mt_ctx_selftest;

struct mt_ctx_selftest_side {
    struct mt_ctx_selftest *st;
    mt_ctx ctx;
    int idx;
};

struct mt_ctx_selftest {
    mt_ctx main;
    struct mt_ctx_selftest_side side[2];
    long count;
    long limit;
    long clobbered;
};

/* The two contexts bounce between each other until count reaches limit; the
 * one that reaches it swaps back to the caller.  Each keeps a value in a
 * callee-saved register and an FP value across every swap, and checks them
 * when it is resumed.  The rounding mode is checked on amd64, where mxcsr is
 * part of the saved state. */
static void
S_mt_ctx_selftest_entry(void *arg)
{
    struct mt_ctx_selftest_side *me = (struct mt_ctx_selftest_side *)arg;
    struct mt_ctx_selftest *st = me->st;
    struct mt_ctx_selftest_side *peer = &st->side[1 - me->idx];
    unsigned long key = MT_CTX_SELFTEST_KEY * (unsigned long)(me->idx + 1);
    double fkey = 1.5 + (double)me->idx;

#if defined(__x86_64__) && defined(__linux__)
    fesetround(FE_UPWARD);
#endif
    for (;;) {
        st->count++;
        if (st->count >= st->limit)
            mt_ctx_swap(&me->ctx, &st->main);
        else
            mt_ctx_swap(&me->ctx, &peer->ctx);
        if (key != MT_CTX_SELFTEST_KEY * (unsigned long)(me->idx + 1)
            || fkey != 1.5 + (double)me->idx)
            st->clobbered++;
#if defined(__x86_64__) && defined(__linux__)
        if (fegetround() != FE_UPWARD)
            st->clobbered++;
#endif
    }
}

static void
S_mt_ctx_selftest_unmap(void *map[2], size_t len)
{
    int i;
    for (i = 0; i < 2; i++)
        if (map[i])
            munmap(map[i], len);
}

XS(XS_MT_Ctx_selftest);
XS(XS_MT_Ctx_selftest)
{
    dXSARGS;
    IV n;
    struct mt_ctx_selftest st;
    void *map[2] = { NULL, NULL };
    size_t pg, len;
    long count;
    int i;
#if defined(__x86_64__) && defined(__linux__)
    int mode;
#endif

    if (items != 1)
        croak_xs_usage(cv, "n");
    n = SvIV(ST(0));
    if (n < 1) {
        ST(0) = sv_2mortal(newSViv(0));
        XSRETURN(1);
    }
    if (Perl_mt_ctx_shstk_active())
        croak_mt(MT_SHSTK_UNSUPPORTED);

    pg = (size_t)sysconf(_SC_PAGESIZE);
    len = MT_CTX_SELFTEST_STACK + pg;
    for (i = 0; i < 2; i++) {
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            break;
        map[i] = p;
        if (mprotect(p, pg, PROT_NONE) != 0)
            break;
    }
    if (i < 2) {
        S_mt_ctx_selftest_unmap(map, len);
        croak_mt(MT_CTX_STACK);
    }

    st.count = 0;
    st.limit = (long)n;
    st.clobbered = 0;
    for (i = 0; i < 2; i++) {
        st.side[i].st = &st;
        st.side[i].idx = i;
        mt_ctx_make(&st.side[i].ctx, (char *)map[i] + pg,
                    MT_CTX_SELFTEST_STACK, S_mt_ctx_selftest_entry,
                    &st.side[i]);
    }
#if defined(__x86_64__) && defined(__linux__)
    mode = fegetround();
#endif
    mt_ctx_swap(&st.main, &st.side[0].ctx);
#if defined(__x86_64__) && defined(__linux__)
    if (fegetround() != mode)
        st.clobbered++;
#endif

    count = st.count;
    S_mt_ctx_selftest_unmap(map, len);
    if (st.clobbered)
        croak_mt(MT_CTX_CLOBBERED);
    ST(0) = sv_2mortal(newSViv(count));
    XSRETURN(1);
}

XS(XS_MT_Ctx_shstk_active);
XS(XS_MT_Ctx_shstk_active)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSViv(Perl_mt_ctx_shstk_active()));
    XSRETURN(1);
}

/* ---- MT::SIMD: the level the host offers ---------------------------------- */

/* Indexed by PERL_SIMD_* (simd/perl_simd_dispatch.h) */
static const char * const S_simd_level_names[] = {
    "scalar", "sse2", "avx2", "vsx", "neon"
};

XS(XS_MT_SIMD_level)
{
    dXSARGS;

    if (items != 0) {
        croak_xs_usage(cv, "");
    }

    ST(0) = sv_2mortal(newSViv(PL_simd_level));
    XSRETURN(1);
}

XS(XS_MT_SIMD_level_name)
{
    dXSARGS;

    if (items != 0) {
        croak_xs_usage(cv, "");
    }

    ST(0) = sv_2mortal(newSVpv(S_simd_level_names[PL_simd_level], 0));
    XSRETURN(1);
}

void
Perl_mt_boot_worker(pTHX)
{
    if (!S_mt_workers_inited) {
        MUTEX_INIT(&S_mt_workers_mutex);
        S_mt_workers_inited = 1;
    }
    Perl_mt_fault_init();
    newXS_flags("MT::SIMD::level", XS_MT_SIMD_level, __FILE__, "", 0);
    newXS_flags("MT::SIMD::level_name", XS_MT_SIMD_level_name, __FILE__, "", 0);
    newXS_flags("MT::Worker::spawn", XS_MT_Worker_spawn, __FILE__, "$@", 0);
    newXS_flags("MT::Worker::join",  XS_MT_Worker_join,  __FILE__, "$", 0);
    newXS_flags("MT::Worker::count", XS_MT_Worker_count, __FILE__, "", 0);
    newXS_flags("MT::Worker::is_worker", XS_MT_Worker_is_worker, __FILE__, "", 0);
    newXS_flags("MT::Ctx::selftest", XS_MT_Ctx_selftest, __FILE__, "$", 0);
    newXS_flags("MT::Ctx::shstk_active", XS_MT_Ctx_shstk_active, __FILE__, "", 0);
    Perl_mt_boot_fiber(aTHX);
    Perl_mt_boot_io(aTHX);
    Perl_mt_boot_chan(aTHX);
}

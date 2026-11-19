/*    mt_cap.c
 *
 *    perl5-mt: the capability table (see mt_cap.h) and the MT::Cap core
 *    functions.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_MT_CAP_C
#include "perl.h"
#include "XSUB.h"
#include "mt_fault.h"

#define MT_CAP_DEFAULT_SLOTS  65536

/* ---- the table ------------------------------------------------------------ */

/* The table is created by the main interpreter alone: at the freeze
 * (perl_freeze_boot_image, before the image clone is made, so every
 * worker is born after it and sees it through the clone) or on the main
 * interpreter's first use when nothing is frozen.  A worker never
 * constructs it, so the read below needs no synchronisation. */
static perl_mt_cap_table_t *
S_mt_cap_table(pTHX)
{
    perl_mt_cap_table_t *t = PL_mt_caps;
    if (t)
        return t;
    if (PL_mt_worker_id != 0)
        Perl_croak(aTHX_ "perl5-mt: no capability table in this worker "
                         "(MT_WORKER_FAILED)");
    {
        const char * const e = PerlEnv_getenv("PERL_MT_CAP_SLOTS");
        const long n = e && *e ? atol(e) : 0;
        uint32_t i;
        t = (perl_mt_cap_table_t *)PerlMemShared_calloc(1, sizeof *t);
        t->nslots = n > 0 ? (uint32_t)n : MT_CAP_DEFAULT_SLOTS;
        if (perl_krt_shm_create(&t->shm,
                                (size_t)t->nslots * sizeof(perl_cap_desc_t),
                                0) != 0) {
            /* no shared memory on this platform: ordinary memory */
            Zero(&t->shm, 1, perl_krt_shm_t);
            t->slots = (perl_cap_desc_t *)
                PerlMemShared_calloc(t->nslots, sizeof(perl_cap_desc_t));
        }
        else
            t->slots = (perl_cap_desc_t *)t->shm.base;
        for (i = 0; i < t->nslots; i++) {
            t->slots[i].next_free = i + 1 < t->nslots ? i + 1 : UINT32_MAX;
            t->slots[i].gen = 1;
        }
        t->free_head = 0;
        MUTEX_INIT(&t->mutex);
        t->ninboxes = 16;
        t->inboxes = (perl_mt_inbox_t *)
            PerlMemShared_calloc(t->ninboxes, sizeof(perl_mt_inbox_t));
        t->inboxes[0].alive = TRUE;              /* the main interpreter */
        PL_mt_caps = t;
    }
    return t;
}

/* Creates the capability table if it does not exist yet; called by
 * perl_freeze_boot_image in the main interpreter before the image is
 * cloned, so that no worker ever has to. */
void
Perl_mt_cap_init(pTHX)
{
    (void)S_mt_cap_table(aTHX);
}

/* owner inboxes; the table mutex is held */
static perl_mt_inbox_t *
S_mt_inbox(perl_mt_cap_table_t *t, uint32_t owner)
{
    if (owner >= t->ninboxes) {
        uint32_t n = t->ninboxes;
        while (n <= owner)
            n *= 2;
        t->inboxes = (perl_mt_inbox_t *)
            PerlMemShared_realloc(t->inboxes, n * sizeof(perl_mt_inbox_t));
        Zero(t->inboxes + t->ninboxes, n - t->ninboxes, perl_mt_inbox_t);
        t->ninboxes = n;
    }
    return &t->inboxes[owner];
}

static void
S_mt_inbox_push(perl_mt_cap_table_t *t, uint32_t owner, uint32_t slot)
{
    perl_mt_inbox_t *ib = S_mt_inbox(t, owner);
    if (!ib->alive && owner != 0) {
        /* the owner is gone: the main interpreter reclaims for it */
        t->rerouted++;
        ib = S_mt_inbox(t, 0);
    }
    if (ib->n == ib->cap) {
        ib->cap = ib->cap ? ib->cap * 2 : 64;
        ib->slots = (uint32_t *)PerlMemShared_realloc(ib->slots,
                                                      ib->cap * sizeof(uint32_t));
    }
    ib->slots[ib->n++] = slot;
}

/* a slot whose last handle went: reclaim it (the table mutex is held) */
static void
S_mt_cap_reclaim(perl_mt_cap_table_t *t, uint32_t slot)
{
    perl_cap_desc_t * const d = &t->slots[slot];
    perl_mt_value_t * const body = (perl_mt_value_t *)
        atomic_exchange_explicit(&d->body, NULL, memory_order_acq_rel);
    if (body)
        Perl_mt_value_free(body);
    d->gen++;
    if (d->gen == 0)
        d->gen = 1;
    d->kind = 0;
    d->owner_id = 0;
    d->next_free = t->free_head;
    t->free_head = slot;
    t->used--;
    t->destroyed++;
}

/*
=for apidoc_section $embedding

=for apidoc mt_cap_mint

Creates a capability owned by this worker whose body is a copy of the
plain scalar C<value> (a reference is refused with C<MT_UNSHAREABLE_REF>)
and returns a new handle to it: a reference to a scalar carrying
C<PERL_MAGIC_mt_cap>, blessed into C<MT::Cap>.

=cut
*/

/* the handle record is perl_mt_cap_ref_t in mt_cap.h (shared with the
 * remote handles of the XS dispatcher, mt_xs_compartment.c) */
#define MT_CAP_REF(mg) ((perl_mt_cap_ref_t *)(mg)->mg_ptr)

static SV *
S_mt_cap_new_handle(pTHX_ uint32_t slot, uint32_t gen)
{
    SV * const inner = newSV_type(SVt_PVMG);
    SV *rv;
    MAGIC *mg;
    perl_mt_cap_ref_t ref;
    Zero(&ref, 1, perl_mt_cap_ref_t);
    ref.kind = MT_CAP_KIND_SCALAR;
    ref.slot = slot;
    ref.gen = gen;
    mg = sv_magicext(inner, NULL, PERL_MAGIC_mt_cap, &PL_vtbl_mt_cap,
                     (const char *)&ref, (I32)sizeof ref);
    mg->mg_flags |= MGf_DUP;
    rv = newRV_noinc(inner);
    sv_bless(rv, gv_stashpvs("MT::Cap", GV_ADD));
    return rv;
}

SV *
Perl_mt_cap_mint(pTHX_ SV *value)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_mt_value_t *body;
    uint32_t slot, gen;
    perl_cap_desc_t *d;

    PERL_ARGS_ASSERT_MT_CAP_MINT;

    body = (perl_mt_value_t *)PerlMemShared_calloc(1, sizeof *body);
    Perl_mt_value_from_sv(aTHX_ body, value);     /* may croak */

    MUTEX_LOCK(&t->mutex);
    slot = t->free_head;
    if (slot == UINT32_MAX) {
        MUTEX_UNLOCK(&t->mutex);
        Perl_mt_value_free(body);
        Perl_mt_fault_record(MT_FAULT_CAP_EXHAUSTED, MT_FAULT_RECOVERABLE,
                             PL_mt_worker_id ? MT_UNIT_WORKER : MT_UNIT_MAIN,
                             PL_mt_worker_id, 1u, MT_WORKER_FAILED,
                             "capability table full");
        Perl_croak(aTHX_ "perl5-mt: capability table full (%" UVuf
                         " slots) (MT_WORKER_FAILED)", (UV)t->nslots);
    }
    d = &t->slots[slot];
    t->free_head = d->next_free;
    d->next_free = UINT32_MAX;
    d->owner_id = PL_mt_worker_id;
    d->kind = MT_CAP_KIND_SCALAR;
    gen = d->gen;
    atomic_store_explicit(&d->body, body, memory_order_release);
    atomic_store_explicit(&d->cap_count, 1u, memory_order_release);
    t->used++;
    t->minted++;
    MUTEX_UNLOCK(&t->mutex);

    return S_mt_cap_new_handle(aTHX_ slot, gen);
}

/* the slot and generation of a handle, or -1 when sv is not one */
static int
S_mt_cap_of(pTHX_ SV *sv, uint32_t *slot, uint32_t *gen, MAGIC **mgp)
{
    MAGIC *mg;
    if (!sv)
        return -1;
    if (SvROK(sv))
        sv = SvRV(sv);
    if (!SvMAGICAL(sv) || !(mg = mg_findext(sv, PERL_MAGIC_mt_cap, &PL_vtbl_mt_cap)))
        return -1;
    if (MT_CAP_REF(mg)->kind != MT_CAP_KIND_SCALAR)
        return -1;                        /* a remote handle (mt_xs) */
    *slot = MT_CAP_REF(mg)->slot;
    *gen = MT_CAP_REF(mg)->gen;
    if (mgp)
        *mgp = mg;
    return 0;
}

/*
=for apidoc mt_cap_is_handle

Whether C<sv> (or what it refers to) is a capability handle.

=for apidoc mt_cap_handle_new

Creates a handle for slot C<slot> at generation C<gen>, for a handle that
has crossed from another worker (the sender already counted it).

=for apidoc mt_cap_share

Counts one more handle on the capability C<handle> refers to and returns
its slot and generation, for sending it to another worker.  Dies with
C<MT_STALE_CAP> when the handle is stale.

=cut
*/

bool
Perl_mt_cap_is_handle(pTHX_ SV *sv)
{
    uint32_t slot, gen;
    PERL_ARGS_ASSERT_MT_CAP_IS_HANDLE;
    return S_mt_cap_of(aTHX_ sv, &slot, &gen, NULL) == 0;
}

SV *
Perl_mt_cap_handle_new(pTHX_ U32 slot, U32 gen)
{
    return S_mt_cap_new_handle(aTHX_ slot, gen);
}

static perl_cap_desc_t *
S_mt_cap_live(pTHX_ perl_mt_cap_table_t *t, SV *handle, uint32_t *slotp,
              uint32_t *genp)
{
    uint32_t slot, gen;
    perl_cap_desc_t *d;
    if (S_mt_cap_of(aTHX_ handle, &slot, &gen, NULL) != 0)
        Perl_croak(aTHX_ "perl5-mt: not a capability handle (MT_STALE_CAP)");
    if (slot >= t->nslots)
        Perl_croak_mt(aTHX_ MT_STALE_CAP);
    d = &t->slots[slot];
    if (d->gen != gen || !atomic_load_explicit(&d->body, memory_order_acquire))
        Perl_croak_mt(aTHX_ MT_STALE_CAP);
    if (slotp) *slotp = slot;
    if (genp) *genp = gen;
    return d;
}

void
Perl_mt_cap_share(pTHX_ SV *handle, U32 *slot, U32 *gen)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_cap_desc_t * const d = S_mt_cap_live(aTHX_ t, handle, slot, gen);
    PERL_ARGS_ASSERT_MT_CAP_SHARE;
    atomic_fetch_add_explicit(&d->cap_count, 1u, memory_order_acq_rel);
}

/*
=for apidoc mt_cap_get

Returns a new SV holding the value of the capability C<handle> refers to.

=for apidoc mt_cap_count

Returns the number of handles the capability has, across all workers.

=for apidoc mt_cap_drop

Gives up C<handle>: its count goes down (reclaiming the slot through the
owner's inbox when it reaches zero) and the handle becomes stale.

=cut
*/

SV *
Perl_mt_cap_get(pTHX_ SV *handle)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_cap_desc_t * const d = S_mt_cap_live(aTHX_ t, handle, NULL, NULL);
    perl_mt_value_t * const body = (perl_mt_value_t *)
        atomic_load_explicit(&d->body, memory_order_acquire);
    PERL_ARGS_ASSERT_MT_CAP_GET;
    return Perl_mt_value_to_sv(aTHX_ body);
}

U32
Perl_mt_cap_count(pTHX_ SV *handle)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_cap_desc_t * const d = S_mt_cap_live(aTHX_ t, handle, NULL, NULL);
    PERL_ARGS_ASSERT_MT_CAP_COUNT;
    return atomic_load_explicit(&d->cap_count, memory_order_acquire);
}

/* one handle fewer on slot (gen checked); the last one notifies the owner */
static void
S_mt_cap_release(pTHX_ uint32_t slot, uint32_t gen)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_cap_desc_t *d;
    if (slot >= t->nslots)
        return;
    d = &t->slots[slot];
    if (d->gen != gen)
        return;                                   /* stale: not ours */
    if (atomic_fetch_sub_explicit(&d->cap_count, 1u, memory_order_acq_rel) == 1) {
        MUTEX_LOCK(&t->mutex);
        S_mt_inbox_push(t, d->owner_id, slot);
        MUTEX_UNLOCK(&t->mutex);
    }
}

void
Perl_mt_cap_drop(pTHX_ SV *handle)
{
    uint32_t slot, gen;
    MAGIC *mg;
    PERL_ARGS_ASSERT_MT_CAP_DROP;
    if (S_mt_cap_of(aTHX_ handle, &slot, &gen, &mg) != 0)
        Perl_croak(aTHX_ "perl5-mt: not a capability handle (MT_STALE_CAP)");
    if (gen == 0)
        return;                                   /* already dropped */
    S_mt_cap_release(aTHX_ slot, gen);
    MT_CAP_REF(mg)->gen = 0;                      /* the handle is stale now */
}

/* the magic callbacks (mg_vtable.pl: vtable 'mt_cap') */

int
Perl_magic_freemtcap(pTHX_ SV *sv, MAGIC *mg)
{
    PERL_ARGS_ASSERT_MAGIC_FREEMTCAP;
    PERL_UNUSED_ARG(sv);
    if (MT_CAP_REF(mg)->kind == MT_CAP_KIND_REMOTE)
        Perl_mt_xs_handle_free(aTHX_ MT_CAP_REF(mg));
    else if (MT_CAP_REF(mg)->gen)
        S_mt_cap_release(aTHX_ MT_CAP_REF(mg)->slot, MT_CAP_REF(mg)->gen);
    return 0;
}

int
Perl_magic_dupmtcap(pTHX_ MAGIC *mg, CLONE_PARAMS *param)
{
    /* a cloned handle (ithreads) is one more handle */
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    perl_mt_cap_ref_t * const ref = MT_CAP_REF(mg);   /* already a copy */
    PERL_ARGS_ASSERT_MAGIC_DUPMTCAP;
    PERL_UNUSED_ARG(param);
    if (ref->kind == MT_CAP_KIND_REMOTE) {
        /* an ithreads clone of a remote handle is not counted by the
         * compartment: the copy is stale, and owns no class string */
        ref->ptr = 0;
        ref->rclass = NULL;
        return 0;
    }
    if (ref->gen && ref->slot < t->nslots
        && t->slots[ref->slot].gen == ref->gen)
        atomic_fetch_add_explicit(&t->slots[ref->slot].cap_count, 1u,
                                  memory_order_acq_rel);
    else
        ref->gen = 0;
    return 0;
}

/*
=for apidoc mt_cap_reap

Reclaims the capabilities whose last handle went and whose owner is this
worker (plus, in the main interpreter, those of workers that have gone);
returns how many.  Called at quiescent points: C<MT::Cap::reap>, before
a mint, and when a worker exits.

=for apidoc mt_cap_worker_exit

The capability sweep of a worker that is leaving: its inbox is drained
and future notifications for it go to the main interpreter.

=cut
*/

U32
Perl_mt_cap_reap(pTHX)
{
    perl_mt_cap_table_t * const t = PL_mt_caps;
    perl_mt_inbox_t *ib;
    U32 n = 0;
    if (!t)
        return 0;
    MUTEX_LOCK(&t->mutex);
    ib = S_mt_inbox(t, PL_mt_worker_id);
    while (ib->n) {
        S_mt_cap_reclaim(t, ib->slots[--ib->n]);
        n++;
    }
    MUTEX_UNLOCK(&t->mutex);
    return n;
}

void
Perl_mt_cap_worker_exit(pTHX)
{
    perl_mt_cap_table_t * const t = PL_mt_caps;
    perl_mt_inbox_t *ib;
    if (!t || PL_mt_worker_id == 0)
        return;
    MUTEX_LOCK(&t->mutex);
    ib = S_mt_inbox(t, PL_mt_worker_id);
    while (ib->n)
        S_mt_cap_reclaim(t, ib->slots[--ib->n]);
    ib->alive = FALSE;
    MUTEX_UNLOCK(&t->mutex);
}

void
Perl_mt_cap_worker_start(pTHX)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    MUTEX_LOCK(&t->mutex);
    S_mt_inbox(t, PL_mt_worker_id)->alive = TRUE;
    MUTEX_UNLOCK(&t->mutex);
}

/*
=for apidoc mt_cap_stats

A new hash reference: slots, used, minted, destroyed, pending (in this
worker's inbox), rerouted (notifications for dead owners), worker id.

=cut
*/

SV *
Perl_mt_cap_stats(pTHX)
{
    perl_mt_cap_table_t * const t = S_mt_cap_table(aTHX);
    HV * const hv = newHV();
    MUTEX_LOCK(&t->mutex);
    (void)hv_stores(hv, "slots", newSVuv(t->nslots));
    (void)hv_stores(hv, "used", newSVuv(t->used));
    (void)hv_stores(hv, "minted", newSVuv(t->minted));
    (void)hv_stores(hv, "destroyed", newSVuv(t->destroyed));
    (void)hv_stores(hv, "rerouted", newSVuv(t->rerouted));
    (void)hv_stores(hv, "pending", newSVuv(S_mt_inbox(t, PL_mt_worker_id)->n));
    (void)hv_stores(hv, "worker", newSVuv(PL_mt_worker_id));
    (void)hv_stores(hv, "shm", newSViv(t->shm.base ? 1 : 0));
    MUTEX_UNLOCK(&t->mutex);
    return newRV_noinc((SV *)hv);
}

/* ---- MT::Cap -------------------------------------------------------------- */

XS(XS_MT_Cap_mint);
XS(XS_MT_Cap_mint)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "value");
    (void)Perl_mt_cap_reap(aTHX);
    ST(0) = sv_2mortal(Perl_mt_cap_mint(aTHX_ ST(0)));
    XSRETURN(1);
}

XS(XS_MT_Cap_get);
XS(XS_MT_Cap_get)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "cap");
    ST(0) = sv_2mortal(Perl_mt_cap_get(aTHX_ ST(0)));
    XSRETURN(1);
}

XS(XS_MT_Cap_count);
XS(XS_MT_Cap_count)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "cap");
    ST(0) = sv_2mortal(newSVuv(Perl_mt_cap_count(aTHX_ ST(0))));
    XSRETURN(1);
}

XS(XS_MT_Cap_drop);
XS(XS_MT_Cap_drop)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "cap");
    Perl_mt_cap_drop(aTHX_ ST(0));
    XSRETURN_YES;
}

XS(XS_MT_Cap_reap);
XS(XS_MT_Cap_reap)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(newSVuv(Perl_mt_cap_reap(aTHX)));
    XSRETURN(1);
}

XS(XS_MT_Cap_stats);
XS(XS_MT_Cap_stats)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    ST(0) = sv_2mortal(Perl_mt_cap_stats(aTHX));
    XSRETURN(1);
}

XS(XS_MT_Cap_is_cap);
XS(XS_MT_Cap_is_cap)
{
    dXSARGS;
    if (items != 1)
        croak_xs_usage(cv, "sv");
    ST(0) = Perl_mt_cap_is_handle(aTHX_ ST(0)) ? &PL_sv_yes : &PL_sv_no;
    XSRETURN(1);
}

void
Perl_mt_boot_cap(pTHX)
{
    newXS_flags("MT::Cap::mint",   XS_MT_Cap_mint,   __FILE__, "$", 0);
    newXS_flags("MT::Cap::get",    XS_MT_Cap_get,    __FILE__, "$", 0);
    newXS_flags("MT::Cap::count",  XS_MT_Cap_count,  __FILE__, "$", 0);
    newXS_flags("MT::Cap::drop",   XS_MT_Cap_drop,   __FILE__, "$", 0);
    newXS_flags("MT::Cap::reap",   XS_MT_Cap_reap,   __FILE__, "",  0);
    newXS_flags("MT::Cap::stats",  XS_MT_Cap_stats,  __FILE__, "",  0);
    newXS_flags("MT::Cap::is_cap", XS_MT_Cap_is_cap, __FILE__, "$", 0);
}

/*
 * ex: set ts=8 sts=4 sw=4 et:
 */

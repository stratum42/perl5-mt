/*    mt_image.h
 *
 *    perl5-mt: the sealed boot image shared by every worker interpreter
 *    (built by perl_freeze_boot_image, mt_freeze.c) and the per-worker
 *    tables that worker interpreters keep for it (mt_worker.c).
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_IMAGE_H
#define PERL_MT_IMAGE_H

#include "krt/krt.h"

/* Overlay-validated inline caches and freeze-time specialisation
 * records.  All tables are built once at freeze time and are
 * strictly read-only after perl_freeze_boot_image() completes; workers validate
 * entries in O(1) by checking that their per-worker HV shadow / GP table slot
 * is still NULL and PL_sub_generation matches base_sub_gen. */

struct perl_mt_meth_ic {
    HV  *stash;         /* immortal class stash */
    SV  *meth;          /* method name SV */
    CV  *cv;            /* resolved CV */
    U32  stash_id;      /* HvMT_ID(HvAUX_raw(stash)) */
    U32  gp_id;         /* GvGP_raw(gv)->gp_mt_id, or U32_MAX if RVCV */
    U32  meth_hash;     /* precomputed hash of meth */
};

#define MT_SITE_NONE        0
#define MT_SITE_METH_DEVIRT 1  /* devirtualised Class->meth() at OP_METHOD_NAMED */
#define MT_SITE_CONST_SUB   2  /* 0-arg constant frozen sub call at OP_ENTERSUB  */

struct perl_mt_site_ic {
    const OP *op;       /* key: OP pointer in the frozen optree */
    U8        kind;     /* MT_SITE_* */
    U32       stash_id; /* stash overlay id (MT_SITE_METH_DEVIRT) */
    U32       gp_id;    /* glob overlay id (or U32_MAX if none) */
    HV       *stash;    /* expected immortal stash (MT_SITE_METH_DEVIRT) */
    SV       *sv;       /* resolved CV* or constant SV* */
    SV       *class_sv; /* expected constant class SV* (MT_SITE_METH_DEVIRT) */
};

struct perl_mt_helem_ic {
    HV  *hv;            /* immortal non-stash hash */
    HEK *hek;           /* frozen key HEK */
    SV  *val;           /* immortal value SV */
    U32  hv_id;         /* HvMT_ID(HvAUX_raw(hv)) */
    U32  key_hash;      /* HEK_HASH(hek) */
};

/* The boot image: an interpreter cloned into a sealable arena at freeze
 * time, with every SV marked SVf_IMMORTAL and the per-worker indirection
 * indices assigned.  Workers never run this interpreter; they are fresh
 * interpreters whose PL_defstash, PL_main_root, PL_main_cv and friends
 * point into it. */
struct perl_mt_image {
    PerlInterpreter *interp;     /* the frozen interpreter (never runs)      */
    perl_krt_shm_t   arena;      /* the sealed segment holding the image     */
    bool             sealed;
    bool             ops_in_arena; /* op slabs were allocated in the arena   */
    bool             specialize_enabled; /* 0 when PERL_MT_NO_SPECIALIZE=1   */

    /* indexed by CvMTID(cv): per-worker depth and padlist tables           */
    U32              ncv;
    CV             **cvs;
    /* indexed by GvGP_raw(gv)->gp_mt_id: per-worker GP and GvFLAGS tables  */
    U32              ngp;
    GP             **gps;        /* the template GPs                         */
    GV             **gvs;        /* one GV owning each template GP           */
    /* indexed by the id in xhv_aux_flags: per-worker aux copies and shadows */
    U32              nhv;
    HV             **hvs;

    /* image roots                                                          */
    HV              *defstash;
    OP              *main_root;
    OP              *main_start;
    CV              *main_cv;
    AV              *regex_padav;
    AV              *clone_stashes; /* stashes with a CLONE method (or NULL) */

    /* Overlay-validated inline caches & freeze specialisations */
    U32                      base_sub_gen;
    struct perl_mt_meth_ic  *meth_ic;
    U32                      meth_ic_mask;
    size_t                   n_meth_ic;
    struct perl_mt_site_ic  *site_ic;
    U32                      site_ic_mask;
    size_t                   n_spec_const_subs;
    size_t                   n_spec_meth_devirt;
    struct perl_mt_helem_ic *helem_ic;
    U32                      helem_ic_mask;
    size_t                   n_helem_ic;

    /* statistics for MT::Image::stats                                      */
    size_t           arena_used;
    size_t           n_sv;
    size_t           n_evicted;
};

#endif /* PERL_MT_IMAGE_H */

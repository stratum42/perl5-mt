/*    mt_xs.h
 *
 *    perl5-mt: the two-tier XS dispatcher.
 *
 *    Tier one is in-process: an XSUB from a module built with
 *    -DPERL_XS_THREADSAFE (XSUB.h emits the marker symbol
 *    mt_threadsafe_<bootname>) is called directly in every worker and is
 *    stamped CVf_MT_THREADSAFE.  Tier two is out of process: a module
 *    without the marker, loaded while workers are active, is loaded into an
 *    XS compartment (a child process forked at the freeze, before any
 *    worker exists) and every XSUB it defines becomes a host stub flagged
 *    CVf_MT_REMOTE whose call Perl_rpp_invoke_xs hands to
 *    Perl_mt_xs_remote_call (mt_xs_bridge.c), which marshals the arguments
 *    into a shared segment, posts a frame on the compartment's request
 *    ring and waits on its own reply ring (mt_xs_compartment.c).
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_XS_H
#define PERL_MT_XS_H

#include "krt/krt.h"
#include "krt/krt_atomic.h"
#include "krt/krt_ring.h"
#include "krt/krt_vec.h"

/* ---- limits and defaults -------------------------------------------------- */

#define MT_XS_MAX_COMPARTMENTS   64      /* pool size cap                       */
#define MT_XS_DEFAULT_COMPARTMENTS 2     /* PERL_MT_XS_COMPARTMENTS             */
#define MT_XS_MAX_SPARES         16      /* pre-forked spare compartments cap   */
#define MT_XS_DEFAULT_SPARES     1       /* PERL_MT_XS_SPARES                   */
#define MT_XS_MAX_SLOTS          256     /* worker slots cap                    */
#define MT_XS_DEFAULT_SLOTS      64      /* PERL_MT_XS_SLOTS                    */
#define MT_XS_DEFAULT_ARENA_MB   8       /* PERL_MT_XS_ARENA_MB, per slot       */
#define MT_XS_REQ_RING_CAP       1024    /* frames per compartment request ring */
#define MT_XS_REPLY_RING_CAP     256     /* frames per worker reply ring        */
#define MT_XS_INLINE_PV          4096    /* PVs above this go as a shm reference */
#define MT_XS_DEFAULT_BATCH      32      /* PERL_MT_XS_BATCH: frames a compartment
                                            serves per wake before it checks its
                                            host (getppid) again                */
#define MT_XS_DEFAULT_INFLIGHT   64      /* PERL_MT_XS_INFLIGHT: remote calls a
                                            worker may have in flight at once    */
#define MT_XS_WAIT_BACKSTOP_NS   1000000000ull  /* a parked waiter's timeout:
                                            the death wake is the signal, this
                                            is only the backstop                 */
#define MT_XS_MAX_DEPTH          512     /* deep copy nesting cap: the encoder
                                            recurses per level and a macOS
                                            worker thread has 512 KiB of stack */

/* ---- frames ----------------------------------------------------------------- */

enum mt_xs_frame_type {
    MT_XSF_NONE = 0,
    /* host -> compartment */
    MT_XSF_LOAD,        /* require a module (or dlopen a .so); report XSUBs   */
    MT_XSF_CALL,        /* call an exported CV                                */
    MT_XSF_REL,         /* release an exported SV (no reply)                  */
    MT_XSF_CB_RET,      /* a host callback returned                           */
    MT_XSF_CB_DIE,      /* a host callback died                               */
    MT_XSF_TIE_FETCH,   /* tied shapes of remote hashes and arrays            */
    MT_XSF_TIE_STORE,
    MT_XSF_TIE_EXISTS,
    MT_XSF_TIE_DELETE,
    MT_XSF_TIE_CLEAR,
    MT_XSF_TIE_FIRSTKEY,
    MT_XSF_TIE_NEXTKEY,
    MT_XSF_TIE_FETCHSIZE,
    MT_XSF_TIE_PUSH,
    MT_XSF_METHOD,      /* method call on a class name or a remote handle     */
    MT_XSF_SETVAR,      /* MT::XS::set_remote_var                             */
    MT_XSF_GETVAR,
    MT_XSF_PING,
    MT_XSF_CRASH,       /* debugging: the compartment aborts                  */
    MT_XSF_SLOT_FREE,   /* a worker slot was released: drop its host handles  */
    /* compartment -> host */
    MT_XSF_RET,         /* results of CALL, LOAD, TIE_x, METHOD, ...         */
    MT_XSF_DIE,         /* the call died                                      */
    MT_XSF_CALLBACK,    /* call a host code ref                               */
    MT_XSF_HREL,        /* release a host code handle                         */
    MT_XSF_MAX
};

/* one ring slot */
typedef struct mt_xs_frame {
    uint32_t type;      /* enum mt_xs_frame_type                               */
    uint32_t slot;      /* the worker slot the frame belongs to                */
    uint32_t seq;       /* call sequence number (matches request and reply)    */
    uint32_t gimme;     /* G_SCALAR / G_LIST / G_VOID for calls                */
    uint64_t off;       /* payload offset in the slot's arena                  */
    uint64_t len;       /* payload length                                      */
    uint64_t a;         /* type specific: CV pointer, SV pointer, ...          */
    uint64_t b;
    uint32_t depth;     /* host-side nesting level of callbacks                */
    uint32_t flags;
    uint64_t pad[2];
} mt_xs_frame_t;

#define MT_XSF_FLAG_PROXY   0x1u   /* METHOD: results are proxy objects      */
#define MT_XSF_FLAG_SO      0x2u   /* LOAD: dlopen the .so, do not require   */
#define MT_XSF_FLAG_PLAIN_DIE 0x4u /* DIE: payload is (u32 len, bytes), not TLV */
#define MT_XSF_FLAG_HOOKS_OK  0x8u /* LOAD: the module may install compiler hooks */
#define MT_XSF_FLAG_AUTOLOAD  0x10u /* CALL: an XS AUTOLOAD; the name leads the payload */
#define MT_XSF_FLAG_COPIED  0x20u  /* host: the payload was copied out of
                                      the arena (a parked fiber decodes it later);
                                      pad[0] is the copy, freed by its caller */

/* ---- the shared segment ----------------------------------------------------- */

#define MT_XS_SHM_MAGIC  0x4d545853u   /* 'MTXS' */

/* per compartment, in shared memory */
typedef struct mt_xs_comp_hdr {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t gen;   /* bumped when it dies      */
    perl_krt_au32_t alive;                        /* set by the child         */
    perl_krt_au32_t pid;
    perl_krt_au32_t calls;                        /* served frames            */
    uint64_t        req_ring_off;                 /* perl_krt_mpsc_t          */
} mt_xs_comp_hdr_t;

/* per worker slot, in shared memory */
typedef struct mt_xs_slot_hdr {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t in_use;
    perl_krt_au32_t worker_id;
    perl_krt_au32_t comp;         /* the compartment this worker is bound to  */
    perl_krt_au32_t bump;         /* arena bump pointer (bytes)               */
    perl_krt_au32_t in_call;      /* the worker is inside a remote call       */
    uint64_t        reply_ring_off;   /* perl_krt_spsc_t                      */
    uint64_t        arena_off;
    uint64_t        arena_len;
    /* Fiber/aio notification hook: the aio context to notify lives here; the
     * reply vector is the signal for the slot. */
    uint64_t        notify_key;
} mt_xs_slot_hdr_t;

/* per pre-forked spare compartment, in shared memory */
typedef struct mt_xs_spare_hdr {
    PERL_KRT_CACHE_ALIGNED perl_krt_au32_t assigned_comp; /* UINT32_MAX while idle */
    perl_krt_au32_t pid;
    perl_krt_au32_t ready;      /* 1 once spare Perl interpreter is ready */
    perl_krt_au32_t shutdown;   /* 1 when host pool is stopping */
} mt_xs_spare_hdr_t;

typedef struct mt_xs_shm_hdr {
    uint32_t magic;
    uint32_t ncomp;
    uint32_t nslots;
    uint32_t nspares;
    uint64_t comp_hdr_off;    /* mt_xs_comp_hdr_t[ncomp]                     */
    uint64_t slot_hdr_off;    /* mt_xs_slot_hdr_t[nslots]                    */
    uint64_t spare_hdr_off;   /* mt_xs_spare_hdr_t[nspares]                  */
    uint64_t vec_off;         /* perl_krt_vectab_t: vector i is compartment
                                 i's request doorbell, vector ncomp + s is
                                 slot s's reply doorbell, vector ncomp + nslots + sp
                                 is spare sp's promotion doorbell            */
    uint64_t total;
} mt_xs_shm_hdr_t;

/* ---- host side -------------------------------------------------------------- */

/* what CvXSUBANY(cv).any_sv holds (as its PV) for a CVf_MT_REMOTE stub */
typedef struct mt_xs_stub {
    uint16_t comp;
    uint16_t gen;
    uint64_t remote_cv;
} mt_xs_stub_t;

/* a remote handle (PERL_MAGIC_mt_cap of kind MT_CAP_KIND_REMOTE) */
typedef struct mt_xs_remote_ref {
    uint16_t comp;
    uint16_t gen;
    uint64_t ptr;             /* the exported SV in the compartment          */
    uint8_t  reftype;         /* SVt_PVHV, SVt_PVAV, SVt_PVCV or SVt_NULL     */
    uint8_t  proxy;           /* object of an MT::XS::remote module          */
    char    *rclass;          /* the real class (malloc'd), or NULL           */
} mt_xs_remote_ref_t;

/* the lazy library handle dl_load_file returns while workers are active */
#define MT_XS_LIB_MAGIC  0x4d544c42u   /* 'MTLB' */
typedef struct mt_xs_lib {
    uint32_t magic;
    uint32_t pad;
    void    *handle;          /* dlopen handle, NULL once remote              */
    char    *path;
    uint32_t comp;            /* the compartment it was loaded into           */
    uint32_t gen;
} mt_xs_lib_t;

/* a remote symbol handle dl_find_symbol returns for boot_X of a legacy .so */
#define MT_XS_SYM_MAGIC  0x4d54534du   /* 'MTSM' */
typedef struct mt_xs_sym {
    uint32_t magic;
    uint32_t pad;
    mt_xs_stub_t stub;
    char    *module;
} mt_xs_sym_t;

#endif /* PERL_MT_XS_H */

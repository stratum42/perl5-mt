/*    mt_cap.h
 *
 *    perl5-mt: capabilities.  A capability is a process-wide slot that
 *    owns one immutable value (its body); handles to it are ordinary Perl
 *    scalars carrying PERL_MAGIC_mt_cap, and may be held by any number of
 *    workers at once.  The slot counts its handles across threads; when
 *    the last handle goes, the owner (the worker that minted the slot, or
 *    the main interpreter) is told through its inbox and reclaims the slot
 *    at its next quiescent point.  Slots carry a generation so that a
 *    handle to a reused slot is detected (MT_STALE_CAP).
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_CAP_H
#define PERL_MT_CAP_H

#include "krt/krt.h"
#include "krt/krt_atomic.h"

/* a value crossing between workers, or the body of a capability */
typedef struct perl_mt_value {
    char    kind;            /* 'u' undef, 'i' IV, 'n' NV, 'p' PV, 'c' cap */
    bool    utf8;
    IV      iv;              /* IV, or the slot of a capability           */
    NV      nv;
    U32     gen;             /* generation of a capability                */
    char   *pv;              /* PerlMemShared, owned                       */
    STRLEN  len;
} perl_mt_value_t;

#define MT_CAP_KIND_SCALAR  1
#define MT_CAP_KIND_REMOTE  2    /* an SV inside an XS compartment */

/* what a handle's PERL_MAGIC_mt_cap carries in mg_ptr (a copy owned by the
 * magic: mg_free frees it and mg_dup copies it before the dup callback) */
typedef struct perl_mt_cap_ref {
    uint32_t slot;            /* scalar capability: the slot ...            */
    uint32_t gen;             /* ... and its generation (0: stale)          */
    uint8_t  kind;            /* MT_CAP_KIND_*                              */
    uint8_t  reftype;         /* remote: SVt_PVHV, SVt_PVAV, SVt_PVCV, else 0 */
    uint8_t  proxy;           /* remote: object of an MT::XS::remote module */
    uint8_t  pad;
    uint16_t comp;            /* remote: the compartment ...                */
    uint16_t cgen;            /* ... at this generation                     */
    uint64_t ptr;             /* remote: the exported SV (0: released)      */
    char    *rclass;          /* remote: the real class (malloc'd) or NULL  */
} perl_mt_cap_ref_t;

typedef struct perl_cap_desc {
    PERL_KRT_CACHE_ALIGNED
    perl_krt_au32_t  cap_count;  /* handles alive, across threads         */
    uint32_t         gen;        /* bumped each time the slot is reused   */
    uint32_t         owner_id;   /* worker id that minted it (0: main)    */
    uint32_t         kind;
    perl_krt_aptr_t  body;       /* perl_mt_value_t *, PerlMemShared     */
    uint32_t         lock;       /* reserved: exclusive mutation */
    uint32_t         next_free;  /* free-list link, UINT32_MAX = none      */
} perl_cap_desc_t;

/* the DESTROY notifications waiting for one owner */
typedef struct perl_mt_inbox {
    uint32_t  *slots;
    uint32_t   n, cap;
    bool       alive;
} perl_mt_inbox_t;

typedef struct perl_mt_cap_table {
    perl_krt_shm_t    shm;         /* the descriptors live here             */
    perl_cap_desc_t  *slots;
    uint32_t          nslots;
    uint32_t          free_head;
    uint32_t          used;        /* slots minted and not yet reclaimed    */
    uint64_t          minted;      /* ever                                  */
    uint64_t          destroyed;   /* reclaimed by their owners             */
    uint64_t          rerouted;    /* notifications sent to a dead owner    */
    perl_mutex        mutex;       /* free list, counters, inboxes          */
    perl_mt_inbox_t  *inboxes;     /* indexed by owner id                   */
    uint32_t          ninboxes;
} perl_mt_cap_table_t;

#endif /* PERL_MT_CAP_H */

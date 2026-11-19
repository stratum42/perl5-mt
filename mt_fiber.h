/*    mt_fiber.h
 *
 *    perl5-mt: fiber records, the interpreter state a fiber swaps,
 *    and the stacklets they run on.  The context switch itself is in
 *    mt_ctx.h; the API is in mt_fiber.c.  This header is included by
 *    mt_fiber.c only, after perl.h.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_FIBER_H
#define PERL_MT_FIBER_H

#include "mt_ctx.h"
#include "mt_xs.h"

/* Under AddressSanitizer every switch is announced to it (mt_fiber.c); the
 * per-context fields exist only in that build. */
#if defined(__SANITIZE_ADDRESS__)
#  define MT_FIBER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define MT_FIBER_ASAN 1
#  endif
#endif

/* The interpreter state a fiber owns while it runs.  Each member is the
 * PL_ variable of the same name (without the prefix; the PL_ names are
 * macros in perl.h and cannot be member names).  The list of fields that
 * MT_VMSTATE_FIELDS names is the whole of it: S_mt_vm_save and S_mt_vm_load
 * in mt_fiber.c copy exactly these and nothing else. */
struct mt_vmstate {
    SV **stack_base;
    SV **stack_sp;
    SV **stack_max;
    AV *curstack;
    PERL_SI *curstackinfo;
    AV *mainstack;
    Stack_off_t *markstack;
    Stack_off_t *markstack_ptr;
    Stack_off_t *markstack_max;
    I32 *scopestack;
    I32 scopestack_ix;
    I32 scopestack_max;
    ANY *savestack;
    I32 savestack_ix;
    I32 savestack_max;
    SV **tmps_stack;
    SSize_t tmps_ix;
    SSize_t tmps_floor;
    SSize_t tmps_max;
    COP *curcop;
    OP *op;
    SV **curpad;
    PAD *comppad;
    JMPENV *top_env;
    OP *restartop;
    JMPENV *restartjmpenv;
    U8 in_eval;
    PMOP *curpm;
    PMOP *curpm_under;
    PMOP *reg_curpm;
    regmatch_slab *rx_slab;     /* PL_regmatch_slab */
    regmatch_state *rx_state;   /* PL_regmatch_state */
    UNOP_AUX_item *multideref_pc;
    U8 localizing;
    U16 delaymagic;
    bool tainted;
    SV *Sv;
    XPV *Xpv;
    STRLEN na;
    Stat_t statcache;
    GV *statgv;
    SV *statname;
    U16 laststype;
    int laststatval;
    SV *rs;
    GV *last_in_gv;
    GV *ofsgv;
    GV *defoutgv;
    SV *ors_sv;
    OP *sortcop;
    HV *sortstash;
    GV *firstgv;
    GV *secondgv;
    SV *errors;
    SV *mess_sv;
    AV *defgv_av;               /* GvAV(PL_defgv) (@_) */
#ifdef DEBUGGING
    const char **scopestack_name;
#endif
};

/* X(field) for every member above that is swapped as a plain copy; the
 * DEBUGGING member and the two regex members are handled alongside in
 * S_mt_vm_save and S_mt_vm_load. */
#define MT_VMSTATE_FIELDS(X)                                                \
    X(stack_base) X(stack_sp) X(stack_max) X(curstack) X(curstackinfo)     \
    X(mainstack) X(markstack) X(markstack_ptr) X(markstack_max)             \
    X(scopestack) X(scopestack_ix) X(scopestack_max)                        \
    X(savestack) X(savestack_ix) X(savestack_max)                           \
    X(tmps_stack) X(tmps_ix) X(tmps_floor) X(tmps_max)                      \
    X(curcop) X(op) X(curpad) X(comppad) X(top_env)                         \
    X(restartop) X(restartjmpenv) X(in_eval)                                \
    X(curpm) X(curpm_under) X(reg_curpm)                                    \
    X(multideref_pc)                                                        \
    X(localizing) X(delaymagic) X(tainted)                                  \
    X(Sv) X(Xpv) X(na)                                                      \
    X(statcache) X(statgv) X(statname) X(laststype) X(laststatval)          \
    X(rs) X(last_in_gv) X(ofsgv) X(defoutgv) X(ors_sv)                      \
    X(sortcop) X(sortstash) X(firstgv) X(secondgv)                          \
    X(errors) X(mess_sv)

/* A fiber, or the record of the thread's original stack (the main context,
 * whose id is 0).  The record is recycled: when a fiber is joined, its
 * record and its stacklet go to the interpreter's free list, and the next
 * spawn takes one back.  The JMPENV that catches a fiber's exit() lives in
 * the trampoline's frame on the fiber's own stack, not in the record: its
 * JMPENV_PUSH names it cur_env. */
struct mt_fiber {
    mt_ctx ctx;
    void *stack_map;            /* guard page + stack, as mapped */
    size_t stack_map_len;
    void *stack_base;           /* usable part, above the guard page */
    size_t stack_len;
    struct mt_vmstate vm;       /* this context's interpreter state, while
                                 * it is not running */
    UV id;                      /* worker id << 32 | sequence; 0 for main */
    U32 flags;                  /* MT_FIBER_* */
    struct mt_fiber *resumer;   /* the context that resumed this one */
    struct mt_fiber *next_free; /* the free list of parked records */
    struct mt_fiber *next_ready; /* the ready queue (MT_FIBER_READY) */
    SV *entry_cv;               /* the code the fiber runs */
    AV *args;                   /* spawn arguments, then the arguments of
                                 * the resume that wakes the fiber */
    SV *result;                 /* yielded values (an RV to an AV) while
                                 * running; the final value once finished */
    SV *error;                  /* $@ of the death, once DIED */
    /* a scheduled fiber parked on a remote call (mt_xs_compartment.c
     * keeps the list of them in PL_mt_xs_waiters).  The drain stores the
     * reply here and wakes the fiber; the fiber copies it out when it runs. */
    U32 xs_wait_seq;            /* the call's sequence number                */
    U32 xs_wait_slot;           /* its worker slot                           */
    U32 xs_wait_comp;           /* the compartment it went to                */
    U32 xs_wait_flags;          /* MT_FIBER_XS_*                             */
    mt_xs_frame_t xs_reply;     /* the reply, once MT_FIBER_XS_DONE           */
    struct mt_fiber *next_xs_waiter;
    /* a scheduled fiber parked on an asynchronous I/O completion
     * (perlio_async.c keeps them in PL_mt_aio_waiters).  The drain stores the
     * completion here and wakes the fiber, which reads it when it runs.      */
    /* a scheduled fiber parked on a channel (mt_chan.c keeps the
     * list in PL_mt_chan_waiters; the drain wakes it when it can go on).   */
    UV chan_id;                 /* the channel's id                           */
    U32 chan_mode;              /* MT_CHAN_MODE_* of the wait                 */
    U32 chan_flags;             /* MT_FIBER_CHAN_DONE                         */
    struct mt_fiber *next_chan_waiter;
    UV aio_key;                 /* user_data of the operation                 */
    U32 aio_flags;              /* MT_FIBER_AIO_*                             */
    perl_krt_cqe_t aio_cqe;     /* the completion, once MT_FIBER_AIO_DONE     */
    struct mt_fiber *next_aio_waiter;
#ifdef MT_FIBER_ASAN
    void *asan_fake;            /* ASan fake-stack handle of this context */
    const void *asan_bottom;    /* the stack of this context, for ASan */
    size_t asan_size;
#endif
};

#define MT_FIBER_RUNNING   0x01   /* resumed and not yet yielded: it is the
                                   * current fiber or one that resumed it */
#define MT_FIBER_DONE      0x02   /* the code has returned or died (the
                                   * error code of that name is separate) */
#define MT_FIBER_DIED      0x04   /* error holds the death; result is unset */
#define MT_FIBER_STARTED   0x08   /* it has a stack and has run once */
/* the scheduler (mt_fiber.c).  A scheduled fiber belongs to the
 * loop of its interpreter: run resumes it, resume refuses it. */
#define MT_FIBER_READY     0x10   /* on the ready queue */
#define MT_FIBER_PARKED    0x20   /* waiting for a wake (not on the queue) */
#define MT_FIBER_SCHEDULED 0x40   /* ready at least once: the loop owns it */

/* Is the fiber `f` a scheduled fiber whose loop `loop` is running?  Such
 * a fiber parks on a remote call instead of blocking its thread. */
#define MT_FIBER_PARKABLE(f, loop) \
    ((loop) && (f) && ((f)->flags & MT_FIBER_SCHEDULED))

/* chan_flags */
#define MT_FIBER_CHAN_DONE 0x01   /* the channel can go on: the waiter resumes */

/* aio_flags */
#define MT_FIBER_AIO_DONE  0x01   /* aio_cqe holds the completion         */

/* xs_wait_flags */
#define MT_FIBER_XS_DONE   0x01   /* xs_reply holds the reply             */
#define MT_FIBER_XS_DEAD   0x02   /* the compartment died: croak on resume */

/* Sizes of a fiber's own Perl stacks, shaped like Perl_init_stacks but
 * smaller: every one of them grows on demand. */
#define MT_FIBER_STACK_ITEMS  32
#define MT_FIBER_CX_ITEMS     16
#define MT_FIBER_MARK_ITEMS   16
#define MT_FIBER_SCOPE_ITEMS  16
#define MT_FIBER_SAVE_ITEMS   32
#define MT_FIBER_TMPS_ITEMS   32

#endif /* PERL_MT_FIBER_H */

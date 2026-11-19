/*    mt_err.h
 *
 *    perl5-mt: the MT_* error codes raised by the multithreaded core
 *    (Perl_croak_mt / Perl_croak_mt_nocontext, util.c).  Plain C, no
 *    dependency on the Perl headers, so that mt_cap.h and the krt tests can
 *    include it too.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_ERR_H
#define PERL_MT_ERR_H

/* Keep PERL_MT_ERR_LIST, the enum and the message table in util.c in sync:
 * the table is generated from the list. */
#define PERL_MT_ERR_LIST(X)                                                 \
    X(MT_OK,                  "no error")                                   \
    X(MT_IMMORTAL_MUTATION,   "attempt to modify an immortal value "        \
                              "(sealed boot image or process constant)")    \
    X(MT_CLOSURE_NOT_SHARED,  "a closure (a sub capturing lexicals) "       \
                              "cannot be shared between workers")           \
    X(MT_STALE_CAP,           "capability handle is stale: its slot has "   \
                              "been reused")                                \
    X(MT_UNSHAREABLE_REF,     "reference to a mutable value cannot be "     \
                              "sent to another worker")                     \
    X(MT_NOT_FROZEN,          "the boot image has not been frozen")         \
    X(MT_ALREADY_FROZEN,      "the boot image is already frozen")           \
    X(MT_FREEZE_FAILED,       "freezing the boot image failed")             \
    X(MT_WORKER_FAILED,       "worker interpreter could not be started")    \
    X(MT_NOT_A_WORKER,        "operation is only valid inside a worker")    \
    X(MT_DEBUGGER_UNSUPPORTED,"breakpoints cannot be set after the boot "   \
                              "image has been frozen")                      \
    X(MT_EXIT_IN_FIBER,       "exit() called inside a fiber")             \
    /* The two-tier XS dispatcher and the XS compartments */               \
    X(MT_STALE_REMOTE_HANDLE, "remote handle is stale: its compartment "   \
                              "has been restarted or has gone")            \
    X(MT_REMOTE_HANDLE_AFFINITY, "remote handle belongs to another "       \
                              "compartment than the XSUB it was given to") \
    X(MT_COMPARTMENT_DEAD,    "XS compartment died while serving a call")  \
    X(MT_NO_COMPARTMENT,      "no XS compartment is available (the pool "  \
                              "is created at the freeze)")                 \
    X(MT_COMPARTMENT_HOOK_UNSUPPORTED, "XS module installed a compiler "   \
                              "hook (PL_check, keyword plugin, peephole, " \
                              "op free hook, glob hook, PerlIO layer or "  \
                              "custom op), which a compartment cannot "    \
                              "serve")                                     \
    X(MT_COMPARTMENT_MAGIC_ON_HOST_SV, "XS module in a compartment "       \
                              "attached magic to an argument that belongs "\
                              "to the host")                               \
    X(MT_REMOTE_UNSUPPORTED_TYPE, "value cannot be marshalled to or from " \
                              "an XS compartment (glob, filehandle, "      \
                              "capability, regexp, or a structure too "    \
                              "deep)")                                     \
    X(MT_REMOTE_FRAME_TOO_LARGE, "marshalled arguments or results exceed " \
                              "the per-worker bridge arena "               \
                              "(PERL_MT_XS_ARENA_MB)")                     \
    X(MT_MY_CXT_WITHOUT_CLONE, "XS module uses MY_CXT but defines no "     \
                              "CLONE method: its per-interpreter state "   \
                              "would be the sealed image's copy in every " \
                              "worker")                                    \
    /* The fiber context switch (mt_ctx.h) */                              \
    X(MT_SHSTK_UNSUPPORTED,   "the x86 shadow stack is active: fibers "    \
                              "cannot run on it yet")                      \
    X(MT_CTX_STACK,           "cannot map the guarded stack of a fiber "   \
                              "context")                                   \
    X(MT_CTX_CLOBBERED,       "a context switch clobbered a callee-saved " \
                              "register")                                  \
    /* Fibers (mt_fiber.c) */                                              \
    X(MT_NOT_A_FIBER,         "no such fiber in this interpreter (joined, "\
                              "or spawned by another worker)")             \
    X(MT_NOT_IN_FIBER,        "operation is only valid inside a fiber")    \
    X(MT_FIBER_FINISHED,      "fiber has already finished")                \
    X(MT_FIBER_BUSY,          "fiber is running: it is the current fiber " \
                              "or one that resumed it")                    \
    X(MT_YIELD_IN_COMPILE,    "fiber yielded or was resumed during "       \
                              "compilation (BEGIN or a string eval)")      \
    /* The fiber scheduler (mt_fiber.c) */                                 \
    X(MT_FIBER_NOT_SCHEDULED, "operation needs a scheduled fiber: this "   \
                              "fiber was resumed explicitly")              \
    X(MT_FIBER_IS_SCHEDULED,  "fiber is scheduled: run resumes it, resume " \
                              "must not")                                  \
    X(MT_FIBER_NOT_FINISHED,  "fiber is scheduled and has not finished: "  \
                              "call run to finish it")                     \
    X(MT_FIBER_DEADLOCK,      "scheduler deadlock: no fiber is ready, "   \
                              "and the parked fibers wait for a wake")     \
    X(MT_SCHED_NESTED,        "a scheduler loop is already running in "    \
                              "this interpreter")                          \
    X(MT_XS_TOO_MANY_INFLIGHT, "too many remote calls in flight from this "  \
                              "worker (PERL_MT_XS_INFLIGHT)")                \
    X(MT_CHAN_CLOSED,         "send on a closed channel")                  \
    X(MT_NO_SUCH_OBJECT,      "no such channel, cell or shared hash (its "  \
                              "id is not from this process)")

enum perl_mt_err {
#define PERL_MT_ERR_ENUM(name, msg) name,
    PERL_MT_ERR_LIST(PERL_MT_ERR_ENUM)
#undef PERL_MT_ERR_ENUM
    MT_ERR_MAX
};

#endif /* PERL_MT_ERR_H */

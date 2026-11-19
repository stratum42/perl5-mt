/*    mt_ctx.h
 *
 *    perl5-mt: the fiber context switch.  The implementations are
 *    per architecture, in mt_ctx_amd64.c, mt_ctx_ppc64le.c,
 *    mt_ctx_ppc64_aix.c, mt_ctx_arm64.c and mt_ctx_s390x.c; each is
 *    self-gated, so all compile on every host and those that do not match
 *    the host are empty.
 *
 *    The contract:
 *
 *    mt_ctx_swap(save, load) saves the callee-saved state of the current
 *    context on the current stack, stores the resulting stack pointer in
 *    save->sp, loads load->sp and returns on the stack it switched to.  A
 *    context that was suspended by a swap resumes from its own swap call.
 *
 *    mt_ctx_make(ctx, stack_base, stack_len, entry, arg) prepares the stack
 *    [stack_base, stack_base + stack_len) so that the first mt_ctx_swap into
 *    ctx calls entry(arg) on it.  The stack grows down from the top; the
 *    initial frame is 16-byte aligned, and entry sees the ABI's stack
 *    alignment at its first instruction.
 *
 *    entry must never return: it must swap away.  The stack must stay
 *    mapped for as long as the context may be resumed, and nothing may
 *    unwind across a swap (no longjmp, no croak, no exception) that would
 *    leave a frame of the other context half-finished.
 *
 *    The swap saves what the ABI makes callee-saved, plus the FP control
 *    state on amd64 (mxcsr and the x87 control word).  It does not save the
 *    signal mask, the TLS pointer or any other kernel-visible state; see
 *    MT_README.md, "The fiber context switch".
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_MT_CTX_H
#define PERL_MT_CTX_H

#include <stddef.h>

#if defined(__x86_64__)
#  define PERL_MT_CTX_ABI "amd64-sysv"
#elif defined(__aarch64__) || defined(__arm64__)
#  define PERL_MT_CTX_ABI "arm64-aapcs64"
#elif defined(_AIX) && (defined(__64BIT__) || defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64))
#  define PERL_MT_CTX_ABI "ppc64-aix-xcoff"
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
#  define PERL_MT_CTX_ABI "ppc64le-elfv2"
#elif defined(__MVS__) && defined(__XPLINK__)
#  define PERL_MT_CTX_ABI "s390x-zos-xplink"
#elif defined(__MVS__)
#  define PERL_MT_CTX_ABI "s390x-zos-std"
#elif defined(__s390x__)
#  define PERL_MT_CTX_ABI "s390x-elf"
#else
#  define PERL_MT_CTX_ABI "unknown"
#endif

typedef struct mt_ctx {
    void *sp;
} mt_ctx;

void mt_ctx_swap(mt_ctx *save, mt_ctx *load);
void mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
                 void (*entry)(void *), void *arg);

/* Nonzero when the kernel has the x86 shadow stack (CET) enabled for this
 * process.  Fibers cannot run on such a process yet (MT_SHSTK_UNSUPPORTED).
 * Defined in mt_worker.c, which needs no per-architecture code for it. */
int Perl_mt_ctx_shstk_active(void);

#endif /* PERL_MT_CTX_H */

/*    mt_ctx_s390x.c
 *
 *    perl5-mt: the fiber context switch on 64-bit z/Architecture
 *    (Linux s390x ELF and z/OS USS 64-bit standard / XPLINK-64 linkage).
 *    The contract is in mt_ctx.h.  Empty on every other architecture.
 *
 *    Two z/Architecture 64-bit linkage modes are supported:
 *
 *      1. Standard 64-bit z/Architecture linkage (Linux s390x ELF and
 *         z/OS non-XPLINK LP64):
 *         r15 is the downward-growing stack pointer, r2/r3 hold save/load,
 *         r14 is the return address, and r6-r15 plus f8-f15 are callee-
 *         saved.  mt_ctx_swap allocates a 160-byte frame, saves r6-r14 at
 *         0..71 and f8-f15 at 80..143, stores r15 in save->sp, loads
 *         load->sp and restores in reverse.
 *
 *      2. z/OS 64-bit XPLINK (__MVS__ && __XPLINK__):
 *         XPLINK-64 uses a biased stack pointer in r4 (+2048), Language
 *         Environment DSA eyecatchers, and CAA stack segment tracking.
 *         To stay 100% compliant with z/OS LE stack unwinding and signal
 *         handling, the XPLINK path stores a POSIX ucontext_t at the top
 *         of the fiber stack and switches via swapcontext().
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__s390x__) || (defined(__MVS__) && (defined(__64BIT__) || defined(_LP64)))

#include <stdint.h>
#include <string.h>
#include "mt_ctx.h"

#if defined(__MVS__) && defined(__XPLINK__)

#ifndef _XOPEN_SOURCE_EXTENDED
#  define _XOPEN_SOURCE_EXTENDED 1
#endif
#include <ucontext.h>

typedef struct mt_ctx_xplink {
    ucontext_t uc;
    void     (*entry)(void *);
    void      *arg;
} mt_ctx_xplink_t;

static void
mt_ctx_xplink_tramp(void *p)
{
    mt_ctx_xplink_t *x = (mt_ctx_xplink_t *)p;
    x->entry(x->arg);
    for (;;)
        ;
}

void
mt_ctx_swap(mt_ctx *save, mt_ctx *load)
{
    mt_ctx_xplink_t *s = (mt_ctx_xplink_t *)save->sp;
    mt_ctx_xplink_t *l = (mt_ctx_xplink_t *)load->sp;
    mt_ctx_xplink_t local;

    if (!s) {
        memset(&local, 0, sizeof local);
        save->sp = &local;
        s = &local;
    }
    (void)swapcontext(&s->uc, &l->uc);
}

void
mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
            void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + stack_len) & ~(uintptr_t)31;
    uintptr_t rec_size = (sizeof(mt_ctx_xplink_t) + 31u) & ~(uintptr_t)31;
    mt_ctx_xplink_t *x = (mt_ctx_xplink_t *)(top - rec_size);

    memset(x, 0, sizeof *x);
    x->entry = entry;
    x->arg   = arg;
    (void)getcontext(&x->uc);
    x->uc.uc_stack.ss_sp   = stack_base;
    x->uc.uc_stack.ss_size = (size_t)((uintptr_t)x - (uintptr_t)stack_base);
    x->uc.uc_link          = NULL;
    makecontext(&x->uc, (void (*)(void))mt_ctx_xplink_tramp, 1, x);
    ctx->sp = x;
}

#else /* Standard 64-bit s390x ELF / z/OS non-XPLINK linkage */

#define MT_CTX_S390X_FRAME 160

#if defined(__MVS__)
#  define MT_CTX_S390X_TYPE(s)
#  define MT_CTX_S390X_SIZE(s)
#else
#  define MT_CTX_S390X_TYPE(s) ".type " #s ", @function\n"
#  define MT_CTX_S390X_SIZE(s) ".size " #s ", .-" #s "\n"
#endif

__asm__(
    ".text\n"
    ".align 8\n"
    ".globl mt_ctx_swap\n"
    MT_CTX_S390X_TYPE(mt_ctx_swap)
    "mt_ctx_swap:\n"
    "aghi %r15, -160\n"
    "stmg %r6, %r14, 0(%r15)\n"
    "std  %f8, 80(%r15)\n"
    "std  %f9, 88(%r15)\n"
    "std  %f10, 96(%r15)\n"
    "std  %f11, 104(%r15)\n"
    "std  %f12, 112(%r15)\n"
    "std  %f13, 120(%r15)\n"
    "std  %f14, 128(%r15)\n"
    "std  %f15, 136(%r15)\n"
    "stg  %r15, 0(%r2)\n"

    "lg   %r15, 0(%r3)\n"
    "ld   %f8, 80(%r15)\n"
    "ld   %f9, 88(%r15)\n"
    "ld   %f10, 96(%r15)\n"
    "ld   %f11, 104(%r15)\n"
    "ld   %f12, 112(%r15)\n"
    "ld   %f13, 120(%r15)\n"
    "ld   %f14, 128(%r15)\n"
    "ld   %f15, 136(%r15)\n"
    "lmg  %r6, %r14, 0(%r15)\n"
    "aghi %r15, 160\n"
    "br   %r14\n"
    MT_CTX_S390X_SIZE(mt_ctx_swap)

    ".align 8\n"
    ".globl mt_ctx_entry_tramp\n"
    MT_CTX_S390X_TYPE(mt_ctx_entry_tramp)
    "mt_ctx_entry_tramp:\n"
    "aghi %r15, -160\n"
    "xc   0(8,%r15), 0(%r15)\n"
    "lgr  %r2, %r6\n"
    "basr %r14, %r7\n"
    ".short 0x0001\n"
    MT_CTX_S390X_SIZE(mt_ctx_entry_tramp)
#if !defined(__MVS__)
    ".section .note.GNU-stack,\"\",@progbits\n"
    ".text\n"
#endif
);

extern void mt_ctx_entry_tramp(void);

void
mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
            void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + stack_len) & ~(uintptr_t)15;
    uintptr_t *frame = (uintptr_t *)(top - MT_CTX_S390X_FRAME);

    memset(frame, 0, MT_CTX_S390X_FRAME);
    frame[0] = (uintptr_t)arg;                      /* r6 */
    frame[1] = (uintptr_t)entry;                    /* r7 */
    frame[8] = (uintptr_t)mt_ctx_entry_tramp;       /* r14 (return address) */
    ctx->sp = frame;
}

#endif /* __MVS__ && __XPLINK__ */

#else
const int perl_mt_ctx_s390x_unused = 0;
#endif /* s390x / z/OS 64-bit */

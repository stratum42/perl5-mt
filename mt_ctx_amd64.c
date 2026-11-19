/*    mt_ctx_amd64.c
 *
 *    perl5-mt: the fiber context switch on amd64 (SysV ABI).  The
 *    contract is in mt_ctx.h.  Empty on every other architecture.
 *
 *    mt_ctx_swap pushes rbp rbx r12-r15, then reserves an 8-byte slot that
 *    holds mxcsr (bytes 0-3) and the x87 control word (bytes 4-5), stores
 *    rsp in save->sp and does the same in reverse for load.  mt_ctx_make
 *    lays out a fresh context in that order, with entry in r12 and arg in
 *    r13; the trampoline moves arg into rdi and calls entry, with rsp a
 *    multiple of 16 before the call.  The word above the trampoline's
 *    frame is zero, so an unwinder stops there.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__x86_64__)

#include <stdint.h>
#include "mt_ctx.h"

#if defined(__APPLE__)
# define MT_CTX_LABEL(s) ".globl _" #s "\n_" #s ":\n"
#else
# define MT_CTX_LABEL(s) ".globl " #s "\n.type " #s ", @function\n" #s ":\n"
#endif

__asm__(
    ".text\n"
    ".p2align 4\n"
    MT_CTX_LABEL(mt_ctx_swap)
    "pushq %rbp\n"
    "pushq %rbx\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "subq $8, %rsp\n"
    "stmxcsr (%rsp)\n"
    "fnstcw 4(%rsp)\n"
    "movq %rsp, (%rdi)\n"
    "movq (%rsi), %rsp\n"
    "ldmxcsr (%rsp)\n"
    "fldcw 4(%rsp)\n"
    "addq $8, %rsp\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %rbx\n"
    "popq %rbp\n"
    "ret\n"

    ".p2align 4\n"
    MT_CTX_LABEL(mt_ctx_entry_tramp)
    "movq %r13, %rdi\n"
    "call *%r12\n"
    "ud2\n"
#if !defined(__APPLE__)
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
    uintptr_t ret_slot = top - 16;   /* 16-aligned: rsp after the ret */
    uintptr_t *frame = (uintptr_t *)(ret_slot - 64);

    /* frame[0]: mxcsr = 0x1F80 (the default), fpu cw = 0x037F */
    frame[0] = 0x1F80 | ((uintptr_t)0x037F << 32);
    frame[1] = 0;                           /* r15 */
    frame[2] = 0;                           /* r14 */
    frame[3] = (uintptr_t)arg;              /* r13 */
    frame[4] = (uintptr_t)entry;            /* r12 */
    frame[5] = 0;                           /* rbx */
    frame[6] = 0;                           /* rbp */
    frame[7] = (uintptr_t)mt_ctx_entry_tramp;
    *(uintptr_t *)ret_slot = 0;             /* the caller that is not there */
    ctx->sp = frame;
}

#endif /* __x86_64__ */

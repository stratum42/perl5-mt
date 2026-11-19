/*    mt_ctx_arm64.c
 *
 *    perl5-mt: the fiber context switch on arm64 (AAPCS64).  The
 *    contract is in mt_ctx.h.  Empty on every other architecture.
 *
 *    mt_ctx_swap stores sp, x19-x28, x29, x30 (raw: this is plain arm64,
 *    not arm64e, so there is no pointer authentication) and d8-d15 in a
 *    160-byte frame, stores sp in save->sp, and does the reverse for load.
 *    x18 is not touched.  The rounding mode (FPCR) is not part of the saved
 *    state; see MT_README.md.
 *
 *    mt_ctx_make lays a fresh frame with entry in x19, arg in x20, a zero
 *    frame pointer and the trampoline's address as x30.  The trampoline
 *    moves arg to x0 and calls entry through x19.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__aarch64__)

#include <stdint.h>
#include <string.h>
#include "mt_ctx.h"

#if defined(__APPLE__)
# define MT_CTX_LABEL(s) ".globl _" #s "\n_" #s ":\n"
# define MT_CTX_ELF(s)
#else
# define MT_CTX_LABEL(s) ".globl " #s "\n.type " #s ", %function\n" #s ":\n"
# define MT_CTX_ELF(s) ".size " #s ", .-" #s "\n"
#endif

#define MT_CTX_FRAME 160

__asm__(
    ".text\n"
    ".p2align 4\n"
    MT_CTX_LABEL(mt_ctx_swap)
    "sub sp, sp, #160\n"
    "stp x19, x20, [sp, #0]\n"
    "stp x21, x22, [sp, #16]\n"
    "stp x23, x24, [sp, #32]\n"
    "stp x25, x26, [sp, #48]\n"
    "stp x27, x28, [sp, #64]\n"
    "stp x29, x30, [sp, #80]\n"
    "stp d8, d9, [sp, #96]\n"
    "stp d10, d11, [sp, #112]\n"
    "stp d12, d13, [sp, #128]\n"
    "stp d14, d15, [sp, #144]\n"
    "mov x2, sp\n"
    "str x2, [x0]\n"
    "ldr x2, [x1]\n"
    "mov sp, x2\n"
    "ldp x19, x20, [sp, #0]\n"
    "ldp x21, x22, [sp, #16]\n"
    "ldp x23, x24, [sp, #32]\n"
    "ldp x25, x26, [sp, #48]\n"
    "ldp x27, x28, [sp, #64]\n"
    "ldp x29, x30, [sp, #80]\n"
    "ldp d8, d9, [sp, #96]\n"
    "ldp d10, d11, [sp, #112]\n"
    "ldp d12, d13, [sp, #128]\n"
    "ldp d14, d15, [sp, #144]\n"
    "add sp, sp, #160\n"
    "ret\n"
    MT_CTX_ELF(mt_ctx_swap)

    ".p2align 4\n"
    MT_CTX_LABEL(mt_ctx_entry_tramp)
    "mov x0, x20\n"
    "blr x19\n"
    "brk #0\n"
    MT_CTX_ELF(mt_ctx_entry_tramp)
#if !defined(__APPLE__)
    ".section .note.GNU-stack,\"\",%progbits\n"
    ".text\n"
#endif
);

extern void mt_ctx_entry_tramp(void);

void
mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
            void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + stack_len) & ~(uintptr_t)15;
    uintptr_t *frame = (uintptr_t *)(top - MT_CTX_FRAME);

    memset(frame, 0, MT_CTX_FRAME);
    frame[0] = (uintptr_t)entry;                    /* x19 */
    frame[1] = (uintptr_t)arg;                      /* x20 */
    frame[10] = 0;                                  /* x29: no frame above */
    frame[11] = (uintptr_t)mt_ctx_entry_tramp;      /* x30 */
    ctx->sp = frame;
}

#endif /* __aarch64__ */

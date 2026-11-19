/*    mt_ctx_ppc64le.c
 *
 *    perl5-mt: the fiber context switch on ppc64le, ELFv2.  The
 *    contract is in mt_ctx.h.  Empty unless the compiler targets ELFv2.
 *
 *    mt_ctx_swap builds a 512-byte frame below the caller (the 32-byte ABI
 *    header, then r14-r31, f14-f31 and v20-v31 at 16-byte aligned offsets),
 *    stores the LR into the caller's LR save word, the CR into the header
 *    and r2 (TOC) into its slot, and stores r1 in save->sp.  load reverses
 *    the sequence.  Nothing in swap uses the TOC, so it has no prologue.
 *
 *    mt_ctx_make lays a fresh frame with arg in r14 and entry in r15 and
 *    the trampoline's address as the saved LR.  The trampoline pushes a
 *    32-byte frame, moves arg to r3 and entry to r12, and calls it through
 *    the count register: the callee derives its TOC from r12, as the
 *    global entry point convention requires.  The word at the top of the
 *    initial stack is zero, which ends the back chain.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2

#include <stdint.h>
#include <string.h>
#include "mt_ctx.h"

/* The frame is 512 bytes below the caller's r1: the 32-byte ABI header
 * (back chain, CR at 8, LR at 16 of the caller's frame, TOC at 24), r14-r31
 * at 32, f14-f31 at 176 and v20-v31 at 320 (16-byte aligned). */
#define MT_CTX_FRAME      512
#define MT_CTX_LR         (MT_CTX_FRAME + 16)

__asm__(
    ".abiversion 2\n"
    ".text\n"
    ".globl mt_ctx_swap\n"
    ".type mt_ctx_swap, @function\n"
    ".p2align 4\n"
    "mt_ctx_swap:\n"
    "mflr 0\n"
    "stdu 1,-512(1)\n"
    "std 0,528(1)\n"
    "mfcr 0\n"
    "std 0,8(1)\n"
    "std 2,24(1)\n"
    "std 14,32(1)\n"
    "std 15,40(1)\n"
    "std 16,48(1)\n"
    "std 17,56(1)\n"
    "std 18,64(1)\n"
    "std 19,72(1)\n"
    "std 20,80(1)\n"
    "std 21,88(1)\n"
    "std 22,96(1)\n"
    "std 23,104(1)\n"
    "std 24,112(1)\n"
    "std 25,120(1)\n"
    "std 26,128(1)\n"
    "std 27,136(1)\n"
    "std 28,144(1)\n"
    "std 29,152(1)\n"
    "std 30,160(1)\n"
    "std 31,168(1)\n"
    "stfd 14,176(1)\n"
    "stfd 15,184(1)\n"
    "stfd 16,192(1)\n"
    "stfd 17,200(1)\n"
    "stfd 18,208(1)\n"
    "stfd 19,216(1)\n"
    "stfd 20,224(1)\n"
    "stfd 21,232(1)\n"
    "stfd 22,240(1)\n"
    "stfd 23,248(1)\n"
    "stfd 24,256(1)\n"
    "stfd 25,264(1)\n"
    "stfd 26,272(1)\n"
    "stfd 27,280(1)\n"
    "stfd 28,288(1)\n"
    "stfd 29,296(1)\n"
    "stfd 30,304(1)\n"
    "stfd 31,312(1)\n"
    "li 12,320\n"
    "stvx 20,1,12\n"
    "li 12,336\n"
    "stvx 21,1,12\n"
    "li 12,352\n"
    "stvx 22,1,12\n"
    "li 12,368\n"
    "stvx 23,1,12\n"
    "li 12,384\n"
    "stvx 24,1,12\n"
    "li 12,400\n"
    "stvx 25,1,12\n"
    "li 12,416\n"
    "stvx 26,1,12\n"
    "li 12,432\n"
    "stvx 27,1,12\n"
    "li 12,448\n"
    "stvx 28,1,12\n"
    "li 12,464\n"
    "stvx 29,1,12\n"
    "li 12,480\n"
    "stvx 30,1,12\n"
    "li 12,496\n"
    "stvx 31,1,12\n"
    "std 1,0(3)\n"

    "ld 5,0(4)\n"
    "ld 0,528(5)\n"
    "mtlr 0\n"
    "ld 0,8(5)\n"
    "mtcrf 255,0\n"
    "ld 2,24(5)\n"
    "ld 14,32(5)\n"
    "ld 15,40(5)\n"
    "ld 16,48(5)\n"
    "ld 17,56(5)\n"
    "ld 18,64(5)\n"
    "ld 19,72(5)\n"
    "ld 20,80(5)\n"
    "ld 21,88(5)\n"
    "ld 22,96(5)\n"
    "ld 23,104(5)\n"
    "ld 24,112(5)\n"
    "ld 25,120(5)\n"
    "ld 26,128(5)\n"
    "ld 27,136(5)\n"
    "ld 28,144(5)\n"
    "ld 29,152(5)\n"
    "ld 30,160(5)\n"
    "ld 31,168(5)\n"
    "lfd 14,176(5)\n"
    "lfd 15,184(5)\n"
    "lfd 16,192(5)\n"
    "lfd 17,200(5)\n"
    "lfd 18,208(5)\n"
    "lfd 19,216(5)\n"
    "lfd 20,224(5)\n"
    "lfd 21,232(5)\n"
    "lfd 22,240(5)\n"
    "lfd 23,248(5)\n"
    "lfd 24,256(5)\n"
    "lfd 25,264(5)\n"
    "lfd 26,272(5)\n"
    "lfd 27,280(5)\n"
    "lfd 28,288(5)\n"
    "lfd 29,296(5)\n"
    "lfd 30,304(5)\n"
    "lfd 31,312(5)\n"
    "li 12,320\n"
    "lvx 20,5,12\n"
    "li 12,336\n"
    "lvx 21,5,12\n"
    "li 12,352\n"
    "lvx 22,5,12\n"
    "li 12,368\n"
    "lvx 23,5,12\n"
    "li 12,384\n"
    "lvx 24,5,12\n"
    "li 12,400\n"
    "lvx 25,5,12\n"
    "li 12,416\n"
    "lvx 26,5,12\n"
    "li 12,432\n"
    "lvx 27,5,12\n"
    "li 12,448\n"
    "lvx 28,5,12\n"
    "li 12,464\n"
    "lvx 29,5,12\n"
    "li 12,480\n"
    "lvx 30,5,12\n"
    "li 12,496\n"
    "lvx 31,5,12\n"
    "mr 1,5\n"
    "addi 1,1,512\n"
    "blr\n"
    ".size mt_ctx_swap, .-mt_ctx_swap\n"

    ".globl mt_ctx_entry_tramp\n"
    ".type mt_ctx_entry_tramp, @function\n"
    ".p2align 4\n"
    "mt_ctx_entry_tramp:\n"
    "stdu 1,-32(1)\n"
    "mr 3,14\n"
    "mr 12,15\n"
    "mtctr 12\n"
    "bctrl\n"
    "trap\n"
    ".size mt_ctx_entry_tramp, .-mt_ctx_entry_tramp\n"
    ".section .note.GNU-stack,\"\",@progbits\n"
    ".text\n"
);

extern void mt_ctx_entry_tramp(void);

void
mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
            void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + stack_len) & ~(uintptr_t)15;
    uintptr_t trap_frame = top - 32;    /* the trampoline's r1 after the ret */
    uintptr_t *frame = (uintptr_t *)(trap_frame - MT_CTX_FRAME);

    memset(frame, 0, MT_CTX_FRAME);
    frame[32 / 8] = (uintptr_t)arg;                 /* r14 */
    frame[40 / 8] = (uintptr_t)entry;               /* r15 */
    frame[MT_CTX_LR / 8] = (uintptr_t)mt_ctx_entry_tramp;
    *(uintptr_t *)trap_frame = 0;       /* the back chain that ends here */
    ctx->sp = frame;
}

#endif /* ELFv2 ppc64le */

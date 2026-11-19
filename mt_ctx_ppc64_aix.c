/*    mt_ctx_ppc64_aix.c
 *
 *    perl5-mt: the fiber context switch on 64-bit AIX and IBM i
 *    PASE (big-endian PowerPC64 XCOFF/TOC ABI).  The contract is in
 *    mt_ctx.h.  Empty on every other platform (including Linux ELFv2
 *    ppc64le, which is handled by mt_ctx_ppc64le.c).
 *
 *    AIX 64-bit XCOFF ABI differences from ELFv2:
 *      - The linkage area is 48 bytes (back chain at 0, CR at 8, LR in
 *        caller's frame at +16, compiler/binder words at 24/32, TOC r2
 *        at 40), followed by a 64-byte parameter save area on calls.
 *      - Function pointers point to 3-doubleword XCOFF function descriptors
 *        [code_addr, toc_anchor, env_ptr] in [DS] csects, not raw code.
 *      - r13 is the thread-local storage pointer and is never modified.
 *      - Non-volatile CR fields are CR2..CR4 (restored via mtcrf 0x38,0).
 *      - Vector registers v20..v31 are saved when Altivec/VSX is enabled.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(_AIX) && (defined(__64BIT__) || defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64))

#include <stdint.h>
#include <string.h>
#include "mt_ctx.h"

#if defined(__ALTIVEC__) || defined(__VSX__) || defined(__VEC__)
#  define MT_CTX_AIX_VEC    1
#  define MT_CTX_AIX_FRAME  528   /* 48 hdr + 144 gpr + 144 fpr + 192 vec */
#  define MT_CTX_AIX_FRAME_S "528"
#  define MT_CTX_AIX_LR_S    "544"
#else
#  define MT_CTX_AIX_VEC    0
#  define MT_CTX_AIX_FRAME  336   /* 48 hdr + 144 gpr + 144 fpr           */
#  define MT_CTX_AIX_FRAME_S "336"
#  define MT_CTX_AIX_LR_S    "352"
#endif

#define MT_CTX_AIX_LR       (MT_CTX_AIX_FRAME + 16)

__asm__(
    ".csect .text[PR]\n"
    ".align 4\n"
    ".globl .mt_ctx_swap\n"
    ".mt_ctx_swap:\n"
    "mflr 0\n"
    "stdu 1, -" MT_CTX_AIX_FRAME_S "(1)\n"
    "std 0, " MT_CTX_AIX_LR_S "(1)\n"
    "mfcr 0\n"
    "std 0, 8(1)\n"
    "std 2, 40(1)\n"
    "std 14, 48(1)\n"
    "std 15, 56(1)\n"
    "std 16, 64(1)\n"
    "std 17, 72(1)\n"
    "std 18, 80(1)\n"
    "std 19, 88(1)\n"
    "std 20, 96(1)\n"
    "std 21, 104(1)\n"
    "std 22, 112(1)\n"
    "std 23, 120(1)\n"
    "std 24, 128(1)\n"
    "std 25, 136(1)\n"
    "std 26, 144(1)\n"
    "std 27, 152(1)\n"
    "std 28, 160(1)\n"
    "std 29, 168(1)\n"
    "std 30, 176(1)\n"
    "std 31, 184(1)\n"
    "stfd 14, 192(1)\n"
    "stfd 15, 200(1)\n"
    "stfd 16, 208(1)\n"
    "stfd 17, 216(1)\n"
    "stfd 18, 224(1)\n"
    "stfd 19, 232(1)\n"
    "stfd 20, 240(1)\n"
    "stfd 21, 248(1)\n"
    "stfd 22, 256(1)\n"
    "stfd 23, 264(1)\n"
    "stfd 24, 272(1)\n"
    "stfd 25, 280(1)\n"
    "stfd 26, 288(1)\n"
    "stfd 27, 296(1)\n"
    "stfd 28, 304(1)\n"
    "stfd 29, 312(1)\n"
    "stfd 30, 320(1)\n"
    "stfd 31, 328(1)\n"
#if MT_CTX_AIX_VEC
    "li 12, 336\n"
    "stvx 20, 1, 12\n"
    "li 12, 352\n"
    "stvx 21, 1, 12\n"
    "li 12, 368\n"
    "stvx 22, 1, 12\n"
    "li 12, 384\n"
    "stvx 23, 1, 12\n"
    "li 12, 400\n"
    "stvx 24, 1, 12\n"
    "li 12, 416\n"
    "stvx 25, 1, 12\n"
    "li 12, 432\n"
    "stvx 26, 1, 12\n"
    "li 12, 448\n"
    "stvx 27, 1, 12\n"
    "li 12, 464\n"
    "stvx 28, 1, 12\n"
    "li 12, 480\n"
    "stvx 29, 1, 12\n"
    "li 12, 496\n"
    "stvx 30, 1, 12\n"
    "li 12, 512\n"
    "stvx 31, 1, 12\n"
#endif
    "std 1, 0(3)\n"

    /* Load target context; r13 (TLS) is untouched. */
    "ld 5, 0(4)\n"
    "ld 0, " MT_CTX_AIX_LR_S "(5)\n"
    "mtlr 0\n"
    "ld 0, 8(5)\n"
    "mtcrf 0x38, 0\n"
    "ld 2, 40(5)\n"
    "ld 14, 48(5)\n"
    "ld 15, 56(5)\n"
    "ld 16, 64(5)\n"
    "ld 17, 72(5)\n"
    "ld 18, 80(5)\n"
    "ld 19, 88(5)\n"
    "ld 20, 96(5)\n"
    "ld 21, 104(5)\n"
    "ld 22, 112(5)\n"
    "ld 23, 120(5)\n"
    "ld 24, 128(5)\n"
    "ld 25, 136(5)\n"
    "ld 26, 144(5)\n"
    "ld 27, 152(5)\n"
    "ld 28, 160(5)\n"
    "ld 29, 168(5)\n"
    "ld 30, 176(5)\n"
    "ld 31, 184(5)\n"
    "lfd 14, 192(5)\n"
    "lfd 15, 200(5)\n"
    "lfd 16, 208(5)\n"
    "lfd 17, 216(5)\n"
    "lfd 18, 224(5)\n"
    "lfd 19, 232(5)\n"
    "lfd 20, 240(5)\n"
    "lfd 21, 248(5)\n"
    "lfd 22, 256(5)\n"
    "lfd 23, 264(5)\n"
    "lfd 24, 272(5)\n"
    "lfd 25, 280(5)\n"
    "lfd 26, 288(5)\n"
    "lfd 27, 296(5)\n"
    "lfd 28, 304(5)\n"
    "lfd 29, 312(5)\n"
    "lfd 30, 320(5)\n"
    "lfd 31, 328(5)\n"
#if MT_CTX_AIX_VEC
    "li 12, 336\n"
    "lvx 20, 5, 12\n"
    "li 12, 352\n"
    "lvx 21, 5, 12\n"
    "li 12, 368\n"
    "lvx 22, 5, 12\n"
    "li 12, 384\n"
    "lvx 23, 5, 12\n"
    "li 12, 400\n"
    "lvx 24, 5, 12\n"
    "li 12, 416\n"
    "lvx 25, 5, 12\n"
    "li 12, 432\n"
    "lvx 26, 5, 12\n"
    "li 12, 448\n"
    "lvx 27, 5, 12\n"
    "li 12, 464\n"
    "lvx 28, 5, 12\n"
    "li 12, 480\n"
    "lvx 29, 5, 12\n"
    "li 12, 496\n"
    "lvx 30, 5, 12\n"
    "li 12, 512\n"
    "lvx 31, 5, 12\n"
#endif
    "mr 1, 5\n"
    "addi 1, 1, " MT_CTX_AIX_FRAME_S "\n"
    "blr\n"

    /* Entry trampoline: r14 = arg, r15 = XCOFF function descriptor of entry */
    ".align 4\n"
    ".globl .mt_ctx_entry_tramp\n"
    ".mt_ctx_entry_tramp:\n"
    "stdu 1, -112(1)\n"
    "std 2, 40(1)\n"
    "mr 3, 14\n"
    "ld 0, 0(15)\n"
    "ld 2, 8(15)\n"
    "ld 11, 16(15)\n"
    "mtctr 0\n"
    "bctrl\n"
    "tw 31, 0, 0\n"

    /* XCOFF64 function descriptors in [DS] csects */
    ".csect mt_ctx_swap[DS], 3\n"
    ".globl mt_ctx_swap\n"
    "mt_ctx_swap:\n"
    ".llong .mt_ctx_swap, TOC[tc0], 0\n"

    ".csect mt_ctx_entry_tramp[DS], 3\n"
    ".globl mt_ctx_entry_tramp\n"
    "mt_ctx_entry_tramp:\n"
    ".llong .mt_ctx_entry_tramp, TOC[tc0], 0\n"

    ".csect .text[PR]\n"
);

extern void mt_ctx_entry_tramp(void);

void
mt_ctx_make(mt_ctx *ctx, void *stack_base, size_t stack_len,
            void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + stack_len) & ~(uintptr_t)15;
    uintptr_t trap_frame = top - 48;    /* 48-byte AIX linkage area above swap frame */
    uintptr_t *frame = (uintptr_t *)(trap_frame - MT_CTX_AIX_FRAME);
    const uintptr_t *tramp_desc = (const uintptr_t *)(uintptr_t)mt_ctx_entry_tramp;

    memset(frame, 0, MT_CTX_AIX_FRAME + 48);
    frame[40 / 8] = tramp_desc[1];                  /* r2 (TOC) */
    frame[48 / 8] = (uintptr_t)arg;                 /* r14 */
    frame[56 / 8] = (uintptr_t)entry;               /* r15 (XCOFF descriptor) */
    frame[MT_CTX_AIX_LR / 8] = tramp_desc[0];       /* saved LR -> .mt_ctx_entry_tramp */
    *(uintptr_t *)trap_frame = 0;                   /* terminate back chain */
    ctx->sp = frame;
}

#else
const int perl_mt_ctx_ppc64_aix_unused = 0;
#endif /* AIX 64-bit PowerPC */

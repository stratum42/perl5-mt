/*    krt_wait.h
 *
 *    Short-spin wait on a 32-bit word, the step before a futex / ulock
 *    park.  perl_krt_wait_line returns as soon as *addr != expected, or
 *    when the budget is spent, and makes no system call on the way.  It
 *    uses the best wait the CPU offers (see krt_wait_tier.h):
 *
 *      umwait     amd64 with WAITPKG: umonitor / umwait (C0.1)
 *      pause      amd64 without WAITPKG: pause loop
 *      wfe        arm64: ldaxr arms the exclusive monitor, wfe sleeps
 *                 until a store to the word clears it
 *      ppc-nap    ppc64le: loop at very low SMT priority, restored after
 *      spin       any other target: plain relax loop
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_WAIT_H
#define PERL_KRT_WAIT_H

#include <stdint.h>
#include "krt.h"

typedef enum perl_krt_wait_reason {
    PERL_KRT_WAIT_CHANGED = 0,   /* *addr != expected                      */
    PERL_KRT_WAIT_BUDGET  = 1,   /* the spin budget was spent first        */
    PERL_KRT_WAIT_TIMEOUT = 2    /* perl_krt_vec_wait: parked, timed out   */
} perl_krt_wait_reason_t;

/* Spin until *addr != expected or budget_ns has passed.  The first call
 * calibrates the tick counter (about 2 ms); perl_krt_wait_tier does it
 * eagerly.  A budget of 0 only tests the value.                          */
perl_krt_wait_reason_t
perl_krt_wait_line(const volatile uint32_t *addr, uint32_t expected,
                   uint64_t budget_ns);

/* The tier this process selected: "umwait", "pause", "wfe", "ppc-nap" or
 * "spin".  Calibrates the tick counter as a side effect.                 */
const char *perl_krt_wait_tier(void);

#endif /* PERL_KRT_WAIT_H */

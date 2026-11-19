/*    krt_stubs.c
 *
 *    perl5-mt kernel runtime adapter: compile-clean boundary for the
 *    follow-on targets (AIX, z/OS USS) and anything else unknown.  Every
 *    primitive returns PERL_KRT_ENOSYS; wait/wake degrade to a timed
 *    sleep so callers that loop still make progress.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_AIX) && !defined(__MVS__)

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "krt.h"
#include "krt_internal.h"
#include "krt_wait.h"

uint64_t perl_krt_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return krt_ts_to_ns(&ts);
}

size_t perl_krt_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
}

const char *perl_krt_platform(void)
{
#if defined(_AIX)
    return "aix/stub";
#elif defined(__MVS__)
    return "zos-uss/stub";
#else
    return "unknown/stub";
#endif
}

int perl_krt_shm_create(perl_krt_shm_t *out, size_t size, uint32_t flags)
{ (void)size; (void)flags; if (out) { memset(out, 0, sizeof *out); out->fd = out->ro_fd = -1; } return PERL_KRT_ENOSYS; }
int perl_krt_shm_seal_ro(perl_krt_shm_t *shm, void *addr, size_t len)
{ (void)shm; (void)addr; (void)len; return PERL_KRT_ENOSYS; }
int perl_krt_shm_remap(const perl_krt_shm_t *shm, void **addr_out, int prot)
{ (void)shm; (void)addr_out; (void)prot; return PERL_KRT_ENOSYS; }
int perl_krt_shm_destroy(perl_krt_shm_t *shm)
{ (void)shm; return PERL_KRT_ENOSYS; }
int perl_krt_shm_unmap(void *addr, size_t len)
{ (void)addr; (void)len; return PERL_KRT_ENOSYS; }
int perl_krt_shm_seal_size(perl_krt_shm_t *shm)
{ (void)shm; return PERL_KRT_ENOSYS; }
int perl_krt_key_alloc(perl_krt_key_t *out)
{ if (out) *out = PERL_KRT_KEY_NONE; return PERL_KRT_ENOSYS; }
int perl_krt_key_free(perl_krt_key_t key)
{ (void)key; return PERL_KRT_ENOSYS; }
int perl_krt_key_arm(perl_krt_key_t key, uint32_t flags)
{ (void)key; (void)flags; return PERL_KRT_ENOSYS; }
int perl_krt_shm_bind_key(perl_krt_shm_t *shm, perl_krt_key_t key)
{ (void)shm; (void)key; return PERL_KRT_ENOSYS; }

int perl_krt_comp_spawn(perl_krt_comp_t *out, perl_krt_comp_fn fn, void *arg,
                        const perl_krt_shm_t *segs, size_t n_segs)
{ (void)out; (void)fn; (void)arg; (void)segs; (void)n_segs; return PERL_KRT_ENOSYS; }
int perl_krt_comp_spawn_flags(perl_krt_comp_t *out, perl_krt_comp_fn fn,
                              void *arg, const perl_krt_shm_t *segs,
                              size_t n_segs, uint32_t flags)
{ (void)out; (void)fn; (void)arg; (void)segs; (void)n_segs; (void)flags; return PERL_KRT_ENOSYS; }
int perl_krt_comp_ctl_fd(void) { return -1; }
int perl_krt_comp_reap(perl_krt_comp_t *comp, int *status)
{ (void)comp; (void)status; return PERL_KRT_ENOSYS; }
int perl_krt_comp_reap_flags(perl_krt_comp_t *comp, int *status, uint32_t flags)
{ (void)comp; (void)status; (void)flags; return PERL_KRT_ENOSYS; }
int perl_krt_comp_kill(perl_krt_comp_t *comp, int sig)
{ (void)comp; (void)sig; return PERL_KRT_ENOSYS; }

void perl_krt_wait_u32(volatile uint32_t *seq, uint32_t expected,
                       uint64_t timeout_ns)
{
    struct timespec ts;
    (void)seq; (void)expected;
    if (timeout_ns == PERL_KRT_WAIT_FOREVER || timeout_ns > 1000000ull)
        timeout_ns = 1000000ull;            /* poll at 1 ms */
    krt_ns_to_ts(timeout_ns, &ts);
    nanosleep(&ts, NULL);
}
void perl_krt_wake_u32(volatile uint32_t *seq, uint32_t count)
{ (void)seq; (void)count; }
void perl_krt_wait_u32_flags(volatile uint32_t *seq, uint32_t expected,
                             uint64_t timeout_ns, uint32_t flags)
{ (void)flags; perl_krt_wait_u32(seq, expected, timeout_ns); }
void perl_krt_wake_u32_flags(volatile uint32_t *seq, uint32_t count,
                             uint32_t flags)
{ (void)seq; (void)count; (void)flags; }

int perl_krt_cpu_id(void) { return -1; }
int perl_krt_cpu_pin(int cpu) { (void)cpu; return PERL_KRT_ENOSYS; }
int perl_krt_smt_siblings(int cpu, int *out, int max)
{ (void)cpu; (void)out; (void)max; return PERL_KRT_ENOSYS; }
int perl_krt_numa_node(int cpu) { (void)cpu; return -1; }

/* No CPU-specific wait here: a plain load loop against the budget. */
perl_krt_wait_reason_t
perl_krt_wait_line(const volatile uint32_t *addr, uint32_t expected,
                   uint64_t budget_ns)
{
    uint64_t end = perl_krt_now_ns() + budget_ns;
    for (;;) {
        if (__atomic_load_n((const uint32_t *)addr, __ATOMIC_ACQUIRE) != expected)
            return PERL_KRT_WAIT_CHANGED;
        if (perl_krt_now_ns() >= end)
            return PERL_KRT_WAIT_BUDGET;
        perl_krt_cpu_relax();
    }
}

const char *perl_krt_wait_tier(void) { return "spin"; }
int perl_krt_cpu_count(void)
{ long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }

int perl_krt_aio_init(perl_krt_aio_t *ctx, uint32_t depth, perl_krt_shm_t *reg_buf)
{ (void)depth; (void)reg_buf; if (ctx) memset(ctx, 0, sizeof *ctx); return PERL_KRT_ENOSYS; }
int perl_krt_aio_init_flags(perl_krt_aio_t *ctx, uint32_t depth,
                            perl_krt_shm_t *reg_buf, uint32_t flags)
{ (void)depth; (void)reg_buf; (void)flags; if (ctx) memset(ctx, 0, sizeof *ctx); return PERL_KRT_ENOSYS; }
int perl_krt_aio_register_files(perl_krt_aio_t *ctx, const int *fds, uint32_t n)
{ (void)ctx; (void)fds; (void)n; return PERL_KRT_ENOSYS; }
int perl_krt_aio_submit(perl_krt_aio_t *ctx, const perl_krt_sqe_t *sqes, uint32_t n)
{ (void)ctx; (void)sqes; (void)n; return PERL_KRT_ENOSYS; }
int perl_krt_aio_drain(perl_krt_aio_t *ctx, perl_krt_cqe_t *cqes, uint32_t max,
                       uint64_t timeout_ns)
{ (void)ctx; (void)cqes; (void)max; (void)timeout_ns; return PERL_KRT_ENOSYS; }
int perl_krt_aio_fixed_bufs(const perl_krt_aio_t *ctx)
{ (void)ctx; return 0; }
int perl_krt_aio_ready(const perl_krt_aio_t *ctx)
{ (void)ctx; return 0; }
int perl_krt_aio_wait(perl_krt_aio_t *ctx, uint64_t timeout_ns)
{ (void)ctx; (void)timeout_ns; return PERL_KRT_ENOSYS; }
int perl_krt_aio_notify(perl_krt_aio_t *ctx, uint64_t user_data)
{ (void)ctx; (void)user_data; return PERL_KRT_ENOSYS; }
int perl_krt_aio_wakeup_fd(const perl_krt_aio_t *ctx)
{ (void)ctx; return -1; }
int perl_krt_aio_close(perl_krt_aio_t *ctx)
{ (void)ctx; return PERL_KRT_ENOSYS; }

#else
const int perl_krt_stubs_backend = 0;   /* keeps the object non-empty */
#endif

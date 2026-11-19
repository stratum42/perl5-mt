/*    krt_darwin.c
 *
 *    perl5-mt kernel runtime adapter, Darwin backend (arm64 macOS):
 *      shm        mach_vm_allocate + mach_make_memory_entry_64; sealing via
 *                 mach_vm_protect(set_maximum); fork inheritance via
 *                 VM_INHERIT_SHARE; remap via mach_vm_map of the entry port
 *      comp       fork (the pool is spawned before any worker thread exists)
 *      wait/wake  __ulock_wait2 / __ulock_wake (libsystem_kernel)
 *      cpu id     pthread_cpu_number_np
 *      aio        a libdispatch worker pool runs the operations and posts
 *                 completions into an MPSC ring; a kqueue EVFILT_USER event
 *                 wakes the draining thread.  sendfile serves SPLICE.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__APPLE__)

#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <mach/memory_object_types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "krt.h"
#include "krt_internal.h"
#include "krt_wait_tier.h"

/* private but stable libsystem_kernel entry points */
#define UL_COMPARE_AND_WAIT   1u
#define UL_COMPARE_AND_WAIT_SHARED 3u   /* the word may be waited on from another process */
#define ULF_WAKE_ALL          0x00000100u
#define ULF_NO_ERRNO          0x01000000u
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
                        uint32_t timeout_us);
extern int __ulock_wait2(uint32_t operation, void *addr, uint64_t value,
                         uint64_t timeout_ns, uint64_t value2)
    __attribute__((weak_import));
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
extern int pthread_cpu_number_np(size_t *cpu_number_out)
    __attribute__((weak_import));

/* ---- misc --------------------------------------------------------------- */

uint64_t
perl_krt_now_ns(void)
{
    /* CLOCK_MONOTONIC steps in whole microseconds on Darwin; the raw
     * uptime clock has nanosecond steps (macOS 10.12 and later)         */
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

size_t
perl_krt_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 16384;
}

const char *
perl_krt_platform(void)
{
    return "darwin-arm64/kqueue";
}

static size_t
krt_round_page(size_t n)
{
    size_t p = perl_krt_page_size();
    return (n + p - 1) & ~(p - 1);
}

static int
krt_kern_to_errno(kern_return_t kr)
{
    switch (kr) {
    case KERN_SUCCESS:            return 0;
    case KERN_INVALID_ADDRESS:    return -EFAULT;
    case KERN_INVALID_ARGUMENT:   return -EINVAL;
    case KERN_NO_SPACE:           return -ENOMEM;
    case KERN_RESOURCE_SHORTAGE:  return -ENOMEM;
    case KERN_PROTECTION_FAILURE: return -EACCES;
    case KERN_NO_ACCESS:          return -EACCES;
    default:                      return -EIO;
    }
}

/* ---- 1. shm: Mach memory entries --------------------------------------- */

int
perl_krt_shm_create(perl_krt_shm_t *out, size_t size, uint32_t flags)
{
    mach_vm_address_t addr = 0;
    memory_object_size_t entry_size;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr;

    if (!out || size == 0)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->fd = out->ro_fd = -1;
    size = krt_round_page(size);

    entry_size = size;
    kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
                                   MAP_MEM_NAMED_CREATE | VM_PROT_READ
                                   | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
        return krt_kern_to_errno(kr);
    kr = mach_vm_map(mach_task_self(), &addr, size, 0, VM_FLAGS_ANYWHERE,
                     entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                     VM_PROT_READ | VM_PROT_WRITE,
                     (flags & PERL_KRT_SHM_NOINHERIT) ? VM_INHERIT_NONE
                                                      : VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), entry);
        return krt_kern_to_errno(kr);
    }
    out->base      = (void *)(uintptr_t)addr;
    out->len       = size;
    out->flags     = flags & ~PERL_KRT_SHM_SEALED;
    out->os_handle = (uint64_t)entry;
    return 0;
}

int
perl_krt_shm_seal_ro(perl_krt_shm_t *shm, void *addr, size_t len)
{
    kern_return_t kr;
    if (!shm || !shm->base || !addr || len == 0)
        return -EINVAL;
    if ((char *)addr < (char *)shm->base
        || (char *)addr + len > (char *)shm->base + shm->len)
        return -ERANGE;
    /* lower the maximum protection: no later mprotect can raise it */
    kr = mach_vm_protect(mach_task_self(), (mach_vm_address_t)(uintptr_t)addr,
                         len, TRUE, VM_PROT_READ);
    if (kr != KERN_SUCCESS)
        return krt_kern_to_errno(kr);
    kr = mach_vm_protect(mach_task_self(), (mach_vm_address_t)(uintptr_t)addr,
                         len, FALSE, VM_PROT_READ);
    if (kr != KERN_SUCCESS)
        return krt_kern_to_errno(kr);
    if (addr == shm->base && len == shm->len)
        shm->flags |= PERL_KRT_SHM_SEALED;
    return 0;
}

int
perl_krt_shm_remap(const perl_krt_shm_t *shm, void **addr_out, int prot)
{
    mach_vm_address_t addr = 0;
    vm_prot_t vp = 0;
    kern_return_t kr;

    if (!shm || !addr_out || !shm->os_handle)
        return -EINVAL;
    if ((shm->flags & PERL_KRT_SHM_SEALED) && (prot & PROT_WRITE))
        return -EPERM;
    if (prot & PROT_READ)  vp |= VM_PROT_READ;
    if (prot & PROT_WRITE) vp |= VM_PROT_WRITE;
    kr = mach_vm_map(mach_task_self(), &addr, shm->len, 0, VM_FLAGS_ANYWHERE,
                     (mach_port_t)shm->os_handle, 0, FALSE, vp, vp,
                     VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS)
        return krt_kern_to_errno(kr);
    *addr_out = (void *)(uintptr_t)addr;
    return 0;
}

int
perl_krt_shm_unmap(void *addr, size_t len)
{
    kern_return_t kr;
    if (!addr || len == 0)
        return -EINVAL;
    kr = mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)addr,
                            len);
    return krt_kern_to_errno(kr);
}

/* Darwin has no user-space memory protection keys. */
int perl_krt_shm_seal_size(perl_krt_shm_t *shm)
{ (void)shm; return 0; }
int perl_krt_key_alloc(perl_krt_key_t *out)
{ if (out) *out = PERL_KRT_KEY_NONE; return PERL_KRT_ENOSYS; }
int perl_krt_key_free(perl_krt_key_t key)
{ (void)key; return PERL_KRT_ENOSYS; }
int perl_krt_key_arm(perl_krt_key_t key, uint32_t flags)
{ (void)key; (void)flags; return PERL_KRT_ENOSYS; }
int perl_krt_shm_bind_key(perl_krt_shm_t *shm, perl_krt_key_t key)
{ (void)shm; (void)key; return PERL_KRT_ENOSYS; }

int
perl_krt_shm_destroy(perl_krt_shm_t *shm)
{
    if (!shm)
        return -EINVAL;
    if (shm->base && shm->len)
        mach_vm_deallocate(mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)shm->base, shm->len);
    if (shm->os_handle)
        mach_port_deallocate(mach_task_self(), (mach_port_t)shm->os_handle);
    memset(shm, 0, sizeof *shm);
    shm->fd = shm->ro_fd = -1;
    return 0;
}

/* ---- 2. compartments: fork ---------------------------------------------- */

static int krt_comp_child_ctl = -1;

int
perl_krt_comp_ctl_fd(void)
{
    return krt_comp_child_ctl;
}

int
perl_krt_comp_spawn_flags(perl_krt_comp_t *out, perl_krt_comp_fn fn, void *arg,
                          const perl_krt_shm_t *segs, size_t n_segs,
                          uint32_t flags)
{
    int sv[2];
    pid_t pid;

    (void)segs; (void)n_segs; (void)flags;  /* VM_INHERIT_SHARE regions */
    if (!out || !fn)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->ctl_fd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return KRT_NEG_ERRNO();
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        int e = KRT_NEG_ERRNO();
        close(sv[0]); close(sv[1]);
        return e;
    }
    if (pid == 0) {
        int rc;
        sigset_t all;
        close(sv[0]);
        krt_comp_child_ctl = sv[1];
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        rc = fn(arg);
        _exit(rc & 0xff);
    }
    close(sv[1]);
    out->pid    = pid;
    out->ctl_fd = sv[0];
    return 0;
}

int
perl_krt_comp_spawn(perl_krt_comp_t *out, perl_krt_comp_fn fn, void *arg,
                    const perl_krt_shm_t *segs, size_t n_segs)
{
    return perl_krt_comp_spawn_flags(out, fn, arg, segs, n_segs, 0);
}

int
perl_krt_comp_reap_flags(perl_krt_comp_t *comp, int *status, uint32_t flags)
{
    int st = 0;
    pid_t r;
    if (!comp || comp->pid <= 0)
        return -EINVAL;
    if (comp->exited) {
        if (status) *status = comp->status;
        return 0;
    }
    do {
        r = waitpid(comp->pid, &st,
                    (flags & PERL_KRT_REAP_NOHANG) ? WNOHANG : 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return KRT_NEG_ERRNO();
    if (r == 0)
        return -EAGAIN;
    comp->exited = 1;
    comp->status = WIFEXITED(st) ? WEXITSTATUS(st)
                 : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    if (comp->ctl_fd >= 0) {
        close(comp->ctl_fd);
        comp->ctl_fd = -1;
    }
    if (status)
        *status = comp->status;
    return 0;
}

int
perl_krt_comp_reap(perl_krt_comp_t *comp, int *status)
{
    return perl_krt_comp_reap_flags(comp, status, 0);
}

int
perl_krt_comp_kill(perl_krt_comp_t *comp, int sig)
{
    if (!comp || comp->pid <= 0 || comp->exited)
        return -EINVAL;
    return kill(comp->pid, sig) < 0 ? KRT_NEG_ERRNO() : 0;
}

/* ---- 3. wait / wake: ulock --------------------------------------------- */

void
perl_krt_wait_u32_flags(volatile uint32_t *seq, uint32_t expected,
                        uint64_t timeout_ns, uint32_t flags)
{
    const uint32_t op = ((flags & PERL_KRT_WAIT_SHARED)
                         ? UL_COMPARE_AND_WAIT_SHARED : UL_COMPARE_AND_WAIT)
                        | ULF_NO_ERRNO;
    if (__ulock_wait2) {
        (void)__ulock_wait2(op, (void *)seq, expected,
                            timeout_ns == PERL_KRT_WAIT_FOREVER ? 0 : timeout_ns,
                            0);
    }
    else {
        uint32_t us = 0;
        if (timeout_ns != PERL_KRT_WAIT_FOREVER) {
            uint64_t v = timeout_ns / 1000u;
            us = v == 0 ? 1u : (v > UINT32_MAX ? UINT32_MAX : (uint32_t)v);
        }
        (void)__ulock_wait(op, (void *)seq, expected, us);
    }
}

void
perl_krt_wake_u32_flags(volatile uint32_t *seq, uint32_t count, uint32_t flags)
{
    const uint32_t op = ((flags & PERL_KRT_WAIT_SHARED)
                         ? UL_COMPARE_AND_WAIT_SHARED : UL_COMPARE_AND_WAIT)
                        | ULF_NO_ERRNO;
    if (count == UINT32_MAX) {
        (void)__ulock_wake(op | ULF_WAKE_ALL, (void *)seq, 0);
        return;
    }
    while (count-- > 0) {
        if (__ulock_wake(op, (void *)seq, 0) != 0)
            break;                    /* no more waiters */
    }
}

void
perl_krt_wait_u32(volatile uint32_t *seq, uint32_t expected,
                  uint64_t timeout_ns)
{
    perl_krt_wait_u32_flags(seq, expected, timeout_ns, 0);
}

void
perl_krt_wake_u32(volatile uint32_t *seq, uint32_t count)
{
    perl_krt_wake_u32_flags(seq, count, 0);
}

/* ---- 4. cpu id ---------------------------------------------------------- */

int
perl_krt_cpu_id(void)
{
    if (pthread_cpu_number_np) {
        size_t n = 0;
        if (pthread_cpu_number_np(&n) == 0)
            return (int)n;
    }
    return -1;
}

int
perl_krt_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* Darwin does not pin threads or expose SMT siblings or NUMA nodes.       */

int
perl_krt_cpu_pin(int cpu)
{
    (void)cpu;
    return PERL_KRT_ENOSYS;
}

int
perl_krt_smt_siblings(int cpu, int *out, int max)
{
    (void)cpu; (void)out; (void)max;
    return PERL_KRT_ENOSYS;
}

int
perl_krt_numa_node(int cpu)
{
    (void)cpu;
    return -1;
}

/* ---- 3b. short-spin wait: the tier code is shared, see krt_wait_tier.h ---- */

perl_krt_wait_reason_t
perl_krt_wait_line(const volatile uint32_t *addr, uint32_t expected,
                   uint64_t budget_ns)
{
    return krt_wait_line_impl(addr, expected, budget_ns);
}

const char *
perl_krt_wait_tier(void)
{
    (void)krt_tb_hz();                  /* calibrate the tick counter now */
    return krt_tier_name(krt_tier());
}

/* ---- 5. aio: dispatch pool + kqueue ------------------------------------ */

#define KRT_KQ_IDENT  1u

typedef struct krt_dq {
    int               kq;
    dispatch_queue_t  queue;
    krt_reqtab_t      reqs;
    perl_krt_shm_t    cq_shm;        /* completion ring (MPSC, producers = pool) */
    perl_krt_mpsc_t  *cq_ring;
    perl_krt_shm_t    notify_shm;
    perl_krt_mpsc_t  *notify_ring;
    int              *files; uint32_t nfiles;
} krt_dq_t;

typedef struct krt_job {
    krt_dq_t        *d;
    perl_krt_aio_t  *ctx;
    uint32_t         idx;
    perl_krt_sqe_t   q;
} krt_job_t;

typedef struct krt_cq_item {
    uint64_t user_data;
    int32_t  res;
    uint32_t idx;
} krt_cq_item_t;

static void
krt_dq_kick(krt_dq_t *d)
{
    struct kevent ev;
    EV_SET(&ev, KRT_KQ_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    (void)kevent(d->kq, &ev, 1, NULL, 0, NULL);
}

static int
krt_job_fd(krt_job_t *j, int32_t fd)
{
    if (j->q.flags & PERL_KRT_SQE_FIXED_FILE) {
        if (fd < 0 || (uint32_t)fd >= j->d->nfiles)
            return -1;
        return j->d->files[fd];
    }
    return fd;
}

static int32_t
krt_job_run(krt_job_t *j)
{
    const perl_krt_sqe_t *q = &j->q;
    void *buf = (void *)(uintptr_t)q->addr;
    int fd = krt_job_fd(j, q->fd);
    ssize_t r;

    if (q->flags & PERL_KRT_SQE_FIXED_BUF) {
        if (!j->ctx->reg_buf || q->addr + q->len > j->ctx->reg_buf->len)
            return -EINVAL;
        buf = (char *)j->ctx->reg_buf->base + q->addr;
    }
    switch (q->op) {
    case PERL_KRT_AIO_NOP:
        return 0;
    case PERL_KRT_AIO_OPENAT: {
        int o = openat(fd, (const char *)(uintptr_t)q->addr,
                       (int)q->open_flags | O_CLOEXEC, (mode_t)q->mode);
        return o < 0 ? KRT_NEG_ERRNO() : o;
    }
    case PERL_KRT_AIO_STATX: {
        struct stat st;
        const char *path = (const char *)(uintptr_t)q->addr;
        int rc;
        if (!q->addr2)
            return -EINVAL;
        if (!path || !*path)
            rc = fstat(fd, &st);
        else
            rc = fstatat(fd, path, &st, (int)q->open_flags & AT_SYMLINK_NOFOLLOW);
        if (rc < 0)
            return KRT_NEG_ERRNO();
        krt_stat_from_posix((perl_krt_stat_t *)(uintptr_t)q->addr2, &st);
        return 0;
    }
    case PERL_KRT_AIO_READ:
        r = (q->off == (uint64_t)-1) ? read(fd, buf, q->len)
                                     : pread(fd, buf, q->len, (off_t)q->off);
        return r < 0 ? KRT_NEG_ERRNO() : (int32_t)r;
    case PERL_KRT_AIO_WRITE:
        r = (q->off == (uint64_t)-1) ? write(fd, buf, q->len)
                                     : pwrite(fd, buf, q->len, (off_t)q->off);
        return r < 0 ? KRT_NEG_ERRNO() : (int32_t)r;
    case PERL_KRT_AIO_FSYNC:
        return fsync(fd) < 0 ? KRT_NEG_ERRNO() : 0;
    case PERL_KRT_AIO_SPLICE: {
        int in = q->fd, out = q->fd2;
        struct stat st;
        off_t len = (off_t)q->len;
        uint64_t total = 0;
        if (fstat(in, &st) == 0 && S_ISREG(st.st_mode)) {
            off_t off = (q->off == (uint64_t)-1) ? lseek(in, 0, SEEK_CUR)
                                                 : (off_t)q->off;
            if (fstat(out, &st) == 0 && S_ISSOCK(st.st_mode)) {
                if (sendfile(in, out, off, &len, NULL, 0) < 0 && len == 0)
                    return KRT_NEG_ERRNO();
                return (int32_t)len;
            }
        }
        {
            char tmp[65536];
            while (total < q->len) {
                size_t want = q->len - total < sizeof tmp ? q->len - total : sizeof tmp;
                ssize_t n = (q->off == (uint64_t)-1) ? read(in, tmp, want)
                        : pread(in, tmp, want, (off_t)(q->off + total));
                ssize_t w, done = 0;
                if (n < 0) return total ? (int32_t)total : KRT_NEG_ERRNO();
                if (n == 0) break;
                while (done < n) {
                    w = (q->off2 == (uint64_t)-1) ? write(out, tmp + done, n - done)
                        : pwrite(out, tmp + done, n - done, (off_t)(q->off2 + total + done));
                    if (w < 0) return total ? (int32_t)total : KRT_NEG_ERRNO();
                    done += w;
                }
                total += (uint64_t)n;
            }
            return (int32_t)total;
        }
    }
    case PERL_KRT_AIO_SEND_ZC: {
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        r = send(fd, buf, q->len, 0);
        return r < 0 ? KRT_NEG_ERRNO() : (int32_t)r;
    }
    case PERL_KRT_AIO_CLOSE:
        return close(fd) < 0 ? KRT_NEG_ERRNO() : 0;
    default:
        return -EINVAL;
    }
}

static void
krt_job_entry(void *p)
{
    krt_job_t *j = (krt_job_t *)p;
    krt_cq_item_t it;
    it.user_data = j->q.user_data;
    it.res       = krt_job_run(j);
    it.idx       = j->idx;
    /* the ring is sized to the request table: a push never fails */
    while (perl_krt_mpsc_push(j->d->cq_ring, &it, sizeof it) == -EAGAIN)
        perl_krt_cpu_relax();
    krt_dq_kick(j->d);
    free(j);
}

int
perl_krt_aio_init_flags(perl_krt_aio_t *ctx, uint32_t depth,
                        perl_krt_shm_t *reg_buf, uint32_t flags)
{
    krt_dq_t *d;
    struct kevent ev;
    uint32_t cap;
    int rc;

    (void)flags;
    if (!ctx || depth < 2 || depth > 32768)
        return -EINVAL;
    memset(ctx, 0, sizeof *ctx);
    d = (krt_dq_t *)calloc(1, sizeof *d);
    if (!d)
        return -ENOMEM;
    d->kq = -1;
    d->cq_shm.fd = d->cq_shm.ro_fd = d->notify_shm.fd = d->notify_shm.ro_fd = -1;

    for (cap = 2; cap < depth; cap <<= 1) ;
    cap <<= 1;

    d->kq = kqueue();
    if (d->kq < 0) { rc = KRT_NEG_ERRNO(); goto fail; }
    fcntl(d->kq, F_SETFD, FD_CLOEXEC);
    EV_SET(&ev, KRT_KQ_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(d->kq, &ev, 1, NULL, 0, NULL) < 0) { rc = KRT_NEG_ERRNO(); goto fail; }

    d->queue = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    rc = krt_reqtab_init(&d->reqs, depth);
    if (rc) goto fail;

    rc = perl_krt_shm_create(&d->cq_shm,
                             perl_krt_mpsc_bytes(cap, sizeof(krt_cq_item_t)),
                             PERL_KRT_SHM_NOINHERIT);
    if (rc) goto fail;
    d->cq_ring = perl_krt_mpsc_init(d->cq_shm.base, d->cq_shm.len, cap,
                                    sizeof(krt_cq_item_t));
    if (!d->cq_ring) { rc = -EINVAL; goto fail; }

    rc = perl_krt_shm_create(&d->notify_shm,
                             perl_krt_mpsc_bytes(KRT_NOTIFY_RING_CAP, 8), 0);
    if (rc) goto fail;
    d->notify_ring = perl_krt_mpsc_init(d->notify_shm.base, d->notify_shm.len,
                                        KRT_NOTIFY_RING_CAP, 8);
    if (!d->notify_ring) { rc = -EINVAL; goto fail; }

    ctx->depth   = depth;
    ctx->flags   = 0;
    ctx->reg_buf = reg_buf;
    ctx->impl    = d;
    return 0;

fail:
    if (d->cq_shm.base) perl_krt_shm_destroy(&d->cq_shm);
    if (d->notify_shm.base) perl_krt_shm_destroy(&d->notify_shm);
    if (d->kq >= 0) close(d->kq);
    krt_reqtab_free(&d->reqs);
    free(d);
    return rc;
}

int
perl_krt_aio_init(perl_krt_aio_t *ctx, uint32_t depth, perl_krt_shm_t *reg_buf)
{
    return perl_krt_aio_init_flags(ctx, depth, reg_buf, 0);
}

int
perl_krt_aio_register_files(perl_krt_aio_t *ctx, const int *fds, uint32_t n)
{
    krt_dq_t *d;
    int *copy;
    if (!ctx || !ctx->impl || !fds || n == 0)
        return -EINVAL;
    d = (krt_dq_t *)ctx->impl;
    copy = (int *)malloc(n * sizeof(int));
    if (!copy)
        return -ENOMEM;
    memcpy(copy, fds, n * sizeof(int));
    free(d->files);
    d->files = copy;
    d->nfiles = n;
    return 0;
}

int
perl_krt_aio_wakeup_fd(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl) ? ((krt_dq_t *)ctx->impl)->kq : -1;
}

int
perl_krt_aio_submit(perl_krt_aio_t *ctx, const perl_krt_sqe_t *sqes, uint32_t n)
{
    krt_dq_t *d;
    uint32_t i;
    if (!ctx || !ctx->impl || (!sqes && n))
        return -EINVAL;
    d = (krt_dq_t *)ctx->impl;
    for (i = 0; i < n; i++) {
        uint32_t idx;
        krt_req_t *r = krt_req_alloc(&d->reqs, &idx);
        krt_job_t *j;
        if (!r)
            break;
        if (sqes[i].op >= PERL_KRT_AIO_OP_MAX) {
            krt_req_release(&d->reqs, idx);
            break;
        }
        j = (krt_job_t *)malloc(sizeof *j);
        if (!j) {
            krt_req_release(&d->reqs, idx);
            break;
        }
        r->user_data = sqes[i].user_data;
        r->op  = sqes[i].op;
        j->d   = d; j->ctx = ctx; j->idx = idx; j->q = sqes[i];
        dispatch_async_f(d->queue, j, krt_job_entry);
    }
    return (int)i;
}

int
perl_krt_aio_drain(perl_krt_aio_t *ctx, perl_krt_cqe_t *cqes, uint32_t max,
                   uint64_t timeout_ns)
{
    krt_dq_t *d;
    uint32_t n = 0;
    uint64_t deadline = 0;

    if (!ctx || !ctx->impl || !cqes || max == 0)
        return -EINVAL;
    d = (krt_dq_t *)ctx->impl;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER && timeout_ns != 0)
        deadline = perl_krt_now_ns() + timeout_ns;

    for (;;) {
        krt_cq_item_t it;
        struct kevent ev;
        struct timespec ts, *tsp = NULL;
        int r;
        while (n < max && perl_krt_mpsc_pop(d->cq_ring, &it, sizeof it) == 0) {
            cqes[n].user_data = it.user_data;
            cqes[n].res       = it.res;
            cqes[n].flags     = 0;
            krt_req_release(&d->reqs, it.idx);
            n++;
        }
        if (n < max)
            n += (uint32_t)krt_notify_pop(d->notify_ring, cqes + n, max - n);
        if (n || timeout_ns == 0)
            return (int)n;
        if (deadline) {
            uint64_t now = perl_krt_now_ns();
            uint64_t left = deadline > now ? deadline - now : 0;
            if (left == 0)
                return 0;
            krt_ns_to_ts(left, &ts);
            tsp = &ts;
        }
        /* a wake with nothing to deliver (coalesced trigger, EINTR) is
         * spurious: keep waiting until the deadline */
        r = kevent(d->kq, NULL, 0, &ev, 1, tsp);
        if (r < 0 && errno != EINTR)
            return KRT_NEG_ERRNO();
    }
}

/* may the ring use SQE_FIXED_BUF operations?  The job threads
 * address the registered segment directly, so this is the segment's presence. */
int
perl_krt_aio_fixed_bufs(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl && ctx->reg_buf) ? 1 : 0;
}

/* is a completion waiting for perl_krt_aio_drain?  The job
 * threads push to cq_ring before they kick the kqueue, so a count here is
 * the same readiness word Linux reads from the ring. */
int
perl_krt_aio_ready(const perl_krt_aio_t *ctx)
{
    krt_dq_t *d;
    if (!ctx || !ctx->impl)
        return 0;
    d = (krt_dq_t *)ctx->impl;
    return perl_krt_mpsc_count(d->cq_ring) != 0
        || perl_krt_mpsc_count(d->notify_ring) != 0;
}

/* block until a completion is ready or timeout_ns passes.  The
 * kqueue wakes on the job kick; a kick that is consumed here leaves its
 * completion in cq_ring, where the next drain finds it.  Returns 0 or
 * -errno. */
int
perl_krt_aio_wait(perl_krt_aio_t *ctx, uint64_t timeout_ns)
{
    krt_dq_t *d;
    struct kevent ev;
    struct timespec ts, *tsp = NULL;
    int r;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    if (perl_krt_aio_ready(ctx) || timeout_ns == 0)
        return 0;
    d = (krt_dq_t *)ctx->impl;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER) {
        krt_ns_to_ts(timeout_ns, &ts);
        tsp = &ts;
    }
    do {
        r = kevent(d->kq, NULL, 0, &ev, 1, tsp);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? KRT_NEG_ERRNO() : 0;
}

/* combined aio + doorbell wait.  Darwin uses kqueue + ulock rather
 * than io_uring IORING_OP_FUTEX_WAIT, so return PERL_KRT_ENOSYS to trigger
 * the caller's bounded fallback. */
int
perl_krt_aio_wait_doorbell(perl_krt_aio_t *ctx, volatile uint32_t *doorbell,
                           uint32_t expected, uint32_t wait_flags,
                           uint64_t timeout_ns)
{
    (void)ctx;
    (void)doorbell;
    (void)expected;
    (void)wait_flags;
    (void)timeout_ns;
    return PERL_KRT_ENOSYS;
}

int
perl_krt_aio_notify(perl_krt_aio_t *ctx, uint64_t user_data)
{
    krt_dq_t *d;
    int rc;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    d = (krt_dq_t *)ctx->impl;
    rc = perl_krt_mpsc_push(d->notify_ring, &user_data, sizeof user_data);
    if (rc)
        return rc;
    krt_dq_kick(d);
    return 0;
}

int
perl_krt_aio_close(perl_krt_aio_t *ctx)
{
    krt_dq_t *d;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    d = (krt_dq_t *)ctx->impl;
    /* let in-flight jobs finish before tearing the ring down */
    while (d->reqs.in_flight) {
        perl_krt_cqe_t tmp[16];
        if (perl_krt_aio_drain(ctx, tmp, 16, 50000000ull) < 0)
            break;
    }
    perl_krt_shm_destroy(&d->cq_shm);
    perl_krt_shm_destroy(&d->notify_shm);
    if (d->kq >= 0) close(d->kq);
    krt_reqtab_free(&d->reqs);
    free(d->files);
    free(d);
    ctx->impl = NULL;
    return 0;
}

#else
const int perl_krt_darwin_backend = 0;  /* keeps the object non-empty */
#endif /* __APPLE__ */

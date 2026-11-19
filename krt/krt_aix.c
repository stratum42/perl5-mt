/*    krt_aix.c
 *
 *    perl5-mt kernel runtime adapter, AIX 7.x and IBM i PASE backend:
 *      shm        SysV shmget/shmat with mprotect sealing; SHM_RDONLY remap;
 *                 fork inheritance via shared attachment
 *      comp       fork + AF_UNIX SOCK_STREAM control socketpair
 *      wait/wake  256-bucket pthread_cond table for process-private waits;
 *                 timed backoff poll for PERL_KRT_WAIT_SHARED across fork
 *      cpu id     _system_configuration / sysconf / mycpu / bindprocessor
 *                 (with runtime IBM i PASE detection)
 *      aio        pthread worker pool + MPSC completion/notify rings +
 *                 wakeup pipe multiplexed via AIX pollset (or POSIX poll
 *                 on IBM i PASE)
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(_AIX)

#ifndef _ALL_SOURCE
#  define _ALL_SOURCE 1
#endif
#ifndef _THREAD_SAFE
#  define _THREAD_SAFE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/processor.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/systemcfg.h>
#include <sys/thread.h>
#include <sys/types.h>
#include <sys/wait.h>

#if !defined(__PASE__) && !defined(__OS400__)
#  include <sys/pollset.h>
#  define KRT_AIX_HAS_POLLSET 1
#else
#  define KRT_AIX_HAS_POLLSET 0
#endif

#include "krt.h"
#include "krt_internal.h"
#include "krt_wait_tier.h"

/* ---- PASE runtime detection & misc -------------------------------------- */

static perl_krt_au32_t krt_pase_state; /* 0 = unknown, 1 = AIX, 2 = PASE */

static int
krt_is_pase(void)
{
#if defined(__PASE__) || defined(__OS400__)
    return 1;
#else
    uint32_t s = perl_krt_load_relaxed(&krt_pase_state);
    if (s)
        return s == 2u;
    {
        struct stat st;
        int pase = (stat("/QOpenSys/usr/lib/libpase.a", &st) == 0);
        perl_krt_store_relaxed(&krt_pase_state, pase ? 2u : 1u);
        return pase;
    }
#endif
}

uint64_t
perl_krt_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        clock_gettime(CLOCK_REALTIME, &ts);
    return krt_ts_to_ns(&ts);
}

size_t
perl_krt_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    if (p > 0)
        return (size_t)p;
    return 4096u;
}

const char *
perl_krt_platform(void)
{
    if (krt_is_pase())
        return "ibmi-pase/poll";
#if KRT_AIX_HAS_POLLSET
    return "aix-ppc64/pollset";
#else
    return "aix-ppc64/poll";
#endif
}

static size_t
krt_round_page(size_t n)
{
    size_t p = perl_krt_page_size();
    return (n + p - 1u) & ~(p - 1u);
}

/* ---- 1. shared memory: SysV shmget/shmat -------------------------------- */

int
perl_krt_shm_create(perl_krt_shm_t *out, size_t size, uint32_t flags)
{
    int shmid;
    void *addr;

    if (!out || size == 0)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->fd = out->ro_fd = -1;
    size = krt_round_page(size);

    shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | IPC_EXCL | 0600);
    if (shmid < 0)
        return KRT_NEG_ERRNO();

    addr = shmat(shmid, NULL, 0);
    if (addr == (void *)-1) {
        int e = KRT_NEG_ERRNO();
        (void)shmctl(shmid, IPC_RMID, NULL);
        return e;
    }

    out->base      = addr;
    out->len       = size;
    out->flags     = flags & ~PERL_KRT_SHM_SEALED;
    out->os_handle = (uint64_t)(unsigned int)shmid;
    return 0;
}

int
perl_krt_shm_seal_ro(perl_krt_shm_t *shm, void *addr, size_t len)
{
    if (!shm || !shm->base || !addr || len == 0)
        return -EINVAL;
    if ((char *)addr < (char *)shm->base
        || (char *)addr + len > (char *)shm->base + shm->len)
        return -ERANGE;

    if (mprotect(addr, len, PROT_READ) < 0) {
        int e = errno;
        /* Some AIX/PASE segment configurations disallow mprotect on SysV
         * shared memory; for a full-segment seal, still enforce read-only
         * on subsequent remaps via PERL_KRT_SHM_SEALED.                   */
        if (e != EINVAL && e != ENOTSUP)
            return -e;
    }
    if (addr == shm->base && len == shm->len)
        shm->flags |= PERL_KRT_SHM_SEALED;
    return 0;
}

int
perl_krt_shm_remap(const perl_krt_shm_t *shm, void **addr_out, int prot)
{
    void *addr;
    int shmflg;

    if (!shm || !addr_out || !shm->base)
        return -EINVAL;
    if ((shm->flags & PERL_KRT_SHM_SEALED) && (prot & PROT_WRITE))
        return -EPERM;

    shmflg = (prot & PROT_WRITE) ? 0 : SHM_RDONLY;
    addr = shmat((int)shm->os_handle, NULL, shmflg);
    if (addr == (void *)-1)
        return KRT_NEG_ERRNO();
    *addr_out = addr;
    return 0;
}

int
perl_krt_shm_unmap(void *addr, size_t len)
{
    (void)len;
    if (!addr)
        return -EINVAL;
    return shmdt(addr) < 0 ? KRT_NEG_ERRNO() : 0;
}

int
perl_krt_shm_destroy(perl_krt_shm_t *shm)
{
    int shmid;
    if (!shm)
        return -EINVAL;
    shmid = (int)shm->os_handle;
    if (shm->base)
        (void)shmdt(shm->base);
    if (shm->base || shmid > 0)
        (void)shmctl(shmid, IPC_RMID, NULL);
    memset(shm, 0, sizeof *shm);
    shm->fd = shm->ro_fd = -1;
    return 0;
}

int
perl_krt_shm_seal_size(perl_krt_shm_t *shm)
{
    if (!shm || !shm->base)
        return -EINVAL;
    return 0;
}

/* ---- 1b. protection keys: mprotect fallback ----------------------------- */

int perl_krt_key_alloc(perl_krt_key_t *out)
{ if (out) *out = PERL_KRT_KEY_NONE; return PERL_KRT_ENOSYS; }
int perl_krt_key_free(perl_krt_key_t key)
{ (void)key; return PERL_KRT_ENOSYS; }
int perl_krt_key_arm(perl_krt_key_t key, uint32_t flags)
{ (void)key; (void)flags; return PERL_KRT_ENOSYS; }
int perl_krt_shm_bind_key(perl_krt_shm_t *shm, perl_krt_key_t key)
{ (void)shm; (void)key; return PERL_KRT_ENOSYS; }

/* ---- 2. compartments: fork + AF_UNIX socketpair ------------------------- */

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

    (void)segs; (void)n_segs; (void)flags;
    if (!out || !fn)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->ctl_fd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return KRT_NEG_ERRNO();
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        int e = KRT_NEG_ERRNO();
        close(sv[0]);
        close(sv[1]);
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
        if (status)
            *status = comp->status;
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

/* ---- 3. wait / wake: hashed pthread_cond table -------------------------- */

#define KRT_WAIT_BUCKETS 256u

typedef struct krt_wait_bucket {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} krt_wait_bucket_t;

static krt_wait_bucket_t krt_wait_tab[KRT_WAIT_BUCKETS];
static pthread_once_t    krt_wait_once = PTHREAD_ONCE_INIT;

static void
krt_wait_tab_init(void)
{
    uint32_t i;
    for (i = 0; i < KRT_WAIT_BUCKETS; i++) {
        pthread_mutex_init(&krt_wait_tab[i].mu, NULL);
        pthread_cond_init(&krt_wait_tab[i].cv, NULL);
    }
}

static krt_wait_bucket_t *
krt_wait_bucket_for(const volatile uint32_t *seq)
{
    uintptr_t h = ((uintptr_t)seq >> 2);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    pthread_once(&krt_wait_once, krt_wait_tab_init);
    return &krt_wait_tab[h & (KRT_WAIT_BUCKETS - 1u)];
}

void
perl_krt_wait_u32_flags(volatile uint32_t *seq, uint32_t expected,
                        uint64_t timeout_ns, uint32_t flags)
{
    if (!seq)
        return;
    if (flags & PERL_KRT_WAIT_SHARED) {
        /* Cross-process wait on shared memory: poll with capped backoff. */
        uint64_t deadline = (timeout_ns == PERL_KRT_WAIT_FOREVER)
                          ? UINT64_MAX : perl_krt_now_ns() + timeout_ns;
        uint64_t step_ns = 50000ull; /* 50 us */
        for (;;) {
            struct timespec ts;
            uint64_t now, left, sl;
            if (__atomic_load_n((const uint32_t *)seq, __ATOMIC_ACQUIRE) != expected)
                return;
            now = perl_krt_now_ns();
            if (deadline != UINT64_MAX && now >= deadline)
                return;
            left = (deadline == UINT64_MAX) ? step_ns : (deadline - now);
            sl = left < step_ns ? left : step_ns;
            krt_ns_to_ts(sl, &ts);
            nanosleep(&ts, NULL);
            if (step_ns < 1000000ull)
                step_ns <<= 1;
        }
    }

    {
        krt_wait_bucket_t *b = krt_wait_bucket_for(seq);
        pthread_mutex_lock(&b->mu);
        if (__atomic_load_n((const uint32_t *)seq, __ATOMIC_ACQUIRE) == expected) {
            if (timeout_ns == PERL_KRT_WAIT_FOREVER) {
                pthread_cond_wait(&b->cv, &b->mu);
            }
            else {
                struct timespec abs_ts;
                uint64_t abs_ns;
                clock_gettime(CLOCK_REALTIME, &abs_ts);
                abs_ns = krt_ts_to_ns(&abs_ts) + timeout_ns;
                krt_ns_to_ts(abs_ns, &abs_ts);
                (void)pthread_cond_timedwait(&b->cv, &b->mu, &abs_ts);
            }
        }
        pthread_mutex_unlock(&b->mu);
    }
}

void
perl_krt_wake_u32_flags(volatile uint32_t *seq, uint32_t count, uint32_t flags)
{
    krt_wait_bucket_t *b;
    if (!seq || count == 0 || (flags & PERL_KRT_WAIT_SHARED))
        return;
    b = krt_wait_bucket_for(seq);
    pthread_mutex_lock(&b->mu);
    if (count == 1u)
        pthread_cond_signal(&b->cv);
    else
        pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
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

/* ---- 3b. short-spin wait tier ------------------------------------------- */

perl_krt_wait_reason_t
perl_krt_wait_line(const volatile uint32_t *addr, uint32_t expected,
                   uint64_t budget_ns)
{
    return krt_wait_line_impl(addr, expected, budget_ns);
}

const char *
perl_krt_wait_tier(void)
{
    (void)krt_tb_hz();
    return krt_tier_name(krt_tier());
}

/* ---- 4. per-CPU identity ------------------------------------------------ */

int
perl_krt_cpu_id(void)
{
    if (krt_is_pase())
        return -1;
    {
        cpu_t c = mycpu();
        return c >= 0 ? (int)c : -1;
    }
}

int
perl_krt_cpu_count(void)
{
    if (_system_configuration.ncpus > 0)
        return _system_configuration.ncpus;
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? (int)n : 1;
    }
}

int
perl_krt_cpu_pin(int cpu)
{
    if (krt_is_pase())
        return PERL_KRT_ENOSYS;
    if (cpu < 0)
        return -EINVAL;
    return bindprocessor(BINDTHREAD, thread_self(), (cpu_t)cpu) < 0
         ? KRT_NEG_ERRNO() : 0;
}

int
perl_krt_smt_siblings(int cpu, int *out, int max)
{
    int smt = _system_configuration.smt_threads;
    int ncpus = perl_krt_cpu_count();
    int base, i, n = 0;

    if (cpu < 0 || cpu >= ncpus || max < 0)
        return -EINVAL;
    if (smt <= 1)
        return 0;
    base = (cpu / smt) * smt;
    for (i = 0; i < smt && (base + i) < ncpus; i++) {
        int sib = base + i;
        if (sib == cpu)
            continue;
        if (out && n < max)
            out[n] = sib;
        n++;
    }
    return n;
}

int
perl_krt_numa_node(int cpu)
{
    (void)cpu;
    return -1;
}

/* ---- 5. async I/O: pthread pool + pollset/poll + wakeup pipe ------------ */

#define KRT_AIX_POOL_THREADS 4u

typedef struct krt_aix_job {
    struct krt_aix_job *next;
    perl_krt_aio_t     *ctx;
    uint32_t            idx;
    perl_krt_sqe_t      q;
} krt_aix_job_t;

typedef struct krt_aix_cq_item {
    uint64_t user_data;
    int32_t  res;
    uint32_t idx;
} krt_aix_cq_item_t;

typedef struct krt_aix_aio {
    int               wake_pipe[2];
#if KRT_AIX_HAS_POLLSET
    pollset_t         ps;
#endif
    int               use_pollset;
    krt_reqtab_t      reqs;
    perl_krt_shm_t    cq_shm;
    perl_krt_mpsc_t  *cq_ring;
    perl_krt_shm_t    notify_shm;
    perl_krt_mpsc_t  *notify_ring;
    int              *files;
    uint32_t          nfiles;

    pthread_mutex_t   q_mu;
    pthread_cond_t    q_cv;
    krt_aix_job_t    *q_head;
    krt_aix_job_t    *q_tail;
    int               stopping;
    uint32_t          nthreads;
    pthread_t         threads[KRT_AIX_POOL_THREADS];
} krt_aix_aio_t;

static void
krt_aix_kick(krt_aix_aio_t *a)
{
    char b = 1;
    ssize_t r;
    do {
        r = write(a->wake_pipe[1], &b, 1);
    } while (r < 0 && errno == EINTR);
}

static void
krt_aix_drain_pipe(krt_aix_aio_t *a)
{
    char buf[64];
    while (read(a->wake_pipe[0], buf, sizeof buf) > 0)
        ;
}

static int
krt_aix_job_fd(krt_aix_aio_t *a, const perl_krt_sqe_t *q, int32_t fd)
{
    if (q->flags & PERL_KRT_SQE_FIXED_FILE) {
        if (fd < 0 || (uint32_t)fd >= a->nfiles)
            return -1;
        return a->files[fd];
    }
    return fd;
}

static int32_t
krt_aix_job_run(krt_aix_aio_t *a, krt_aix_job_t *j)
{
    const perl_krt_sqe_t *q = &j->q;
    void *buf = (void *)(uintptr_t)q->addr;
    int fd = krt_aix_job_fd(a, q, q->fd);
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
            rc = fstatat(fd, path, &st,
                         (int)q->open_flags & AT_SYMLINK_NOFOLLOW);
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
        uint64_t total = 0;
        char tmp[32768];
        while (total < q->len) {
            size_t want = (q->len - total < sizeof tmp)
                        ? (size_t)(q->len - total) : sizeof tmp;
            ssize_t n = (q->off == (uint64_t)-1) ? read(in, tmp, want)
                      : pread(in, tmp, want, (off_t)(q->off + total));
            ssize_t w, done = 0;
            if (n < 0)
                return total ? (int32_t)total : KRT_NEG_ERRNO();
            if (n == 0)
                break;
            while (done < n) {
                w = (q->off2 == (uint64_t)-1)
                  ? write(out, tmp + done, (size_t)(n - done))
                  : pwrite(out, tmp + done, (size_t)(n - done),
                           (off_t)(q->off2 + total + (uint64_t)done));
                if (w < 0)
                    return total ? (int32_t)total : KRT_NEG_ERRNO();
                done += w;
            }
            total += (uint64_t)n;
        }
        return (int32_t)total;
    }
    case PERL_KRT_AIO_SEND_ZC: {
#ifdef MSG_NOSIGNAL
        int sflags = MSG_NOSIGNAL;
#else
        int sflags = 0;
#endif
        r = send(fd, buf, q->len, sflags);
        return r < 0 ? KRT_NEG_ERRNO() : (int32_t)r;
    }
    case PERL_KRT_AIO_CLOSE:
        return close(fd) < 0 ? KRT_NEG_ERRNO() : 0;
    default:
        return -EINVAL;
    }
}

static void *
krt_aix_worker(void *arg)
{
    krt_aix_aio_t *a = (krt_aix_aio_t *)arg;
    for (;;) {
        krt_aix_job_t *j;
        krt_aix_cq_item_t it;

        pthread_mutex_lock(&a->q_mu);
        while (!a->q_head && !a->stopping)
            pthread_cond_wait(&a->q_cv, &a->q_mu);
        if (!a->q_head && a->stopping) {
            pthread_mutex_unlock(&a->q_mu);
            break;
        }
        j = a->q_head;
        a->q_head = j->next;
        if (!a->q_head)
            a->q_tail = NULL;
        pthread_mutex_unlock(&a->q_mu);

        it.user_data = j->q.user_data;
        it.res       = krt_aix_job_run(a, j);
        it.idx       = j->idx;
        while (perl_krt_mpsc_push(a->cq_ring, &it, sizeof it) == -EAGAIN)
            perl_krt_cpu_relax();
        krt_aix_kick(a);
        free(j);
    }
    return NULL;
}

static int
krt_aix_poll_wake(krt_aix_aio_t *a, int timeout_ms)
{
    struct pollfd pfd;
    int r;

#if KRT_AIX_HAS_POLLSET
    if (a->use_pollset) {
        r = pollset_poll(a->ps, &pfd, 1, timeout_ms);
        if (r > 0)
            krt_aix_drain_pipe(a);
        return r;
    }
#endif
    pfd.fd      = a->wake_pipe[0];
    pfd.events  = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms);
    if (r > 0)
        krt_aix_drain_pipe(a);
    return r;
}

int
perl_krt_aio_init_flags(perl_krt_aio_t *ctx, uint32_t depth,
                        perl_krt_shm_t *reg_buf, uint32_t flags)
{
    krt_aix_aio_t *a;
    uint32_t cap, i;
    int rc;

    (void)flags;
    if (!ctx || depth < 2 || depth > 32768)
        return -EINVAL;
    memset(ctx, 0, sizeof *ctx);
    a = (krt_aix_aio_t *)calloc(1, sizeof *a);
    if (!a)
        return -ENOMEM;
    a->wake_pipe[0] = a->wake_pipe[1] = -1;
#if KRT_AIX_HAS_POLLSET
    a->ps = (pollset_t)-1;
#endif
    a->cq_shm.fd = a->cq_shm.ro_fd = -1;
    a->notify_shm.fd = a->notify_shm.ro_fd = -1;

    pthread_mutex_init(&a->q_mu, NULL);
    pthread_cond_init(&a->q_cv, NULL);

    if (pipe(a->wake_pipe) < 0) {
        rc = KRT_NEG_ERRNO();
        goto fail;
    }
    (void)fcntl(a->wake_pipe[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(a->wake_pipe[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(a->wake_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(a->wake_pipe[1], F_SETFL, O_NONBLOCK);

#if KRT_AIX_HAS_POLLSET
    if (!krt_is_pase()) {
        a->ps = pollset_create(-1);
        if (a->ps != (pollset_t)-1) {
            struct poll_ctl ctl;
            ctl.cmd    = PS_ADD;
            ctl.events = POLLIN;
            ctl.fd     = a->wake_pipe[0];
            if (pollset_ctl(a->ps, &ctl, 1) == 0) {
                a->use_pollset = 1;
            }
            else {
                pollset_destroy(a->ps);
                a->ps = (pollset_t)-1;
            }
        }
    }
#endif

    for (cap = 2; cap < depth; cap <<= 1) ;
    cap <<= 1;

    rc = krt_reqtab_init(&a->reqs, depth);
    if (rc)
        goto fail;

    rc = perl_krt_shm_create(&a->cq_shm,
                             perl_krt_mpsc_bytes(cap, sizeof(krt_aix_cq_item_t)),
                             PERL_KRT_SHM_NOINHERIT);
    if (rc)
        goto fail;
    a->cq_ring = perl_krt_mpsc_init(a->cq_shm.base, a->cq_shm.len, cap,
                                    sizeof(krt_aix_cq_item_t));
    if (!a->cq_ring) {
        rc = -EINVAL;
        goto fail;
    }

    rc = perl_krt_shm_create(&a->notify_shm,
                             perl_krt_mpsc_bytes(KRT_NOTIFY_RING_CAP, 8), 0);
    if (rc)
        goto fail;
    a->notify_ring = perl_krt_mpsc_init(a->notify_shm.base, a->notify_shm.len,
                                        KRT_NOTIFY_RING_CAP, 8);
    if (!a->notify_ring) {
        rc = -EINVAL;
        goto fail;
    }

    for (i = 0; i < KRT_AIX_POOL_THREADS; i++) {
        if (pthread_create(&a->threads[i], NULL, krt_aix_worker, a) != 0) {
            rc = KRT_NEG_ERRNO();
            goto fail;
        }
        a->nthreads++;
    }

    ctx->depth   = depth;
    ctx->flags   = 0;
    ctx->reg_buf = reg_buf;
    ctx->impl    = a;
    return 0;

fail:
    if (a->nthreads) {
        pthread_mutex_lock(&a->q_mu);
        a->stopping = 1;
        pthread_cond_broadcast(&a->q_cv);
        pthread_mutex_unlock(&a->q_mu);
        for (i = 0; i < a->nthreads; i++)
            pthread_join(a->threads[i], NULL);
    }
    if (a->cq_shm.base)
        perl_krt_shm_destroy(&a->cq_shm);
    if (a->notify_shm.base)
        perl_krt_shm_destroy(&a->notify_shm);
    krt_reqtab_free(&a->reqs);
#if KRT_AIX_HAS_POLLSET
    if (a->ps != (pollset_t)-1)
        pollset_destroy(a->ps);
#endif
    if (a->wake_pipe[0] >= 0)
        close(a->wake_pipe[0]);
    if (a->wake_pipe[1] >= 0)
        close(a->wake_pipe[1]);
    pthread_cond_destroy(&a->q_cv);
    pthread_mutex_destroy(&a->q_mu);
    free(a);
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
    krt_aix_aio_t *a;
    int *copy;
    if (!ctx || !ctx->impl || !fds || n == 0)
        return -EINVAL;
    a = (krt_aix_aio_t *)ctx->impl;
    copy = (int *)malloc(n * sizeof(int));
    if (!copy)
        return -ENOMEM;
    memcpy(copy, fds, n * sizeof(int));
    free(a->files);
    a->files  = copy;
    a->nfiles = n;
    return 0;
}

int
perl_krt_aio_wakeup_fd(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl) ? ((krt_aix_aio_t *)ctx->impl)->wake_pipe[0] : -1;
}

int
perl_krt_aio_submit(perl_krt_aio_t *ctx, const perl_krt_sqe_t *sqes, uint32_t n)
{
    krt_aix_aio_t *a;
    uint32_t i;

    if (!ctx || !ctx->impl || (!sqes && n))
        return -EINVAL;
    a = (krt_aix_aio_t *)ctx->impl;

    for (i = 0; i < n; i++) {
        uint32_t idx;
        krt_req_t *r = krt_req_alloc(&a->reqs, &idx);
        krt_aix_job_t *j;
        if (!r)
            break;
        if (sqes[i].op >= PERL_KRT_AIO_OP_MAX) {
            krt_req_release(&a->reqs, idx);
            break;
        }
        j = (krt_aix_job_t *)malloc(sizeof *j);
        if (!j) {
            krt_req_release(&a->reqs, idx);
            break;
        }
        r->user_data = sqes[i].user_data;
        r->op        = sqes[i].op;
        j->next      = NULL;
        j->ctx       = ctx;
        j->idx       = idx;
        j->q         = sqes[i];

        pthread_mutex_lock(&a->q_mu);
        if (a->q_tail)
            a->q_tail->next = j;
        else
            a->q_head = j;
        a->q_tail = j;
        pthread_cond_signal(&a->q_cv);
        pthread_mutex_unlock(&a->q_mu);
    }
    return (int)i;
}

int
perl_krt_aio_drain(perl_krt_aio_t *ctx, perl_krt_cqe_t *cqes, uint32_t max,
                   uint64_t timeout_ns)
{
    krt_aix_aio_t *a;
    uint32_t n = 0;
    uint64_t deadline = 0;

    if (!ctx || !ctx->impl || !cqes || max == 0)
        return -EINVAL;
    a = (krt_aix_aio_t *)ctx->impl;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER && timeout_ns != 0)
        deadline = perl_krt_now_ns() + timeout_ns;

    for (;;) {
        krt_aix_cq_item_t it;
        int timeout_ms = -1;
        int r;

        while (n < max && perl_krt_mpsc_pop(a->cq_ring, &it, sizeof it) == 0) {
            cqes[n].user_data = it.user_data;
            cqes[n].res       = it.res;
            cqes[n].flags     = 0;
            krt_req_release(&a->reqs, it.idx);
            n++;
        }
        if (n < max)
            n += (uint32_t)krt_notify_pop(a->notify_ring, cqes + n, max - n);
        if (n || timeout_ns == 0)
            return (int)n;

        if (deadline) {
            uint64_t now = perl_krt_now_ns();
            uint64_t left = deadline > now ? deadline - now : 0;
            if (left == 0)
                return 0;
            timeout_ms = (int)((left + 999999ull) / 1000000ull);
            if (timeout_ms < 1)
                timeout_ms = 1;
        }
        r = krt_aix_poll_wake(a, timeout_ms);
        if (r < 0 && errno != EINTR)
            return KRT_NEG_ERRNO();
    }
}

int
perl_krt_aio_fixed_bufs(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl && ctx->reg_buf) ? 1 : 0;
}

int
perl_krt_aio_ready(const perl_krt_aio_t *ctx)
{
    krt_aix_aio_t *a;
    if (!ctx || !ctx->impl)
        return 0;
    a = (krt_aix_aio_t *)ctx->impl;
    return perl_krt_mpsc_count(a->cq_ring) != 0
        || perl_krt_mpsc_count(a->notify_ring) != 0;
}

int
perl_krt_aio_wait(perl_krt_aio_t *ctx, uint64_t timeout_ns)
{
    krt_aix_aio_t *a;
    int timeout_ms = -1;
    int r;

    if (!ctx || !ctx->impl)
        return -EINVAL;
    if (perl_krt_aio_ready(ctx) || timeout_ns == 0)
        return 0;
    a = (krt_aix_aio_t *)ctx->impl;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER) {
        timeout_ms = (int)((timeout_ns + 999999ull) / 1000000ull);
        if (timeout_ms < 1)
            timeout_ms = 1;
    }
    r = krt_aix_poll_wake(a, timeout_ms);
    if (r < 0 && errno != EINTR)
        return KRT_NEG_ERRNO();
    return 0;
}

int
perl_krt_aio_notify(perl_krt_aio_t *ctx, uint64_t user_data)
{
    krt_aix_aio_t *a;
    int rc;

    if (!ctx || !ctx->impl)
        return -EINVAL;
    a = (krt_aix_aio_t *)ctx->impl;
    rc = perl_krt_mpsc_push(a->notify_ring, &user_data, sizeof user_data);
    if (rc)
        return rc;
    krt_aix_kick(a);
    return 0;
}

int
perl_krt_aio_close(perl_krt_aio_t *ctx)
{
    krt_aix_aio_t *a;
    uint32_t i;

    if (!ctx || !ctx->impl)
        return -EINVAL;
    a = (krt_aix_aio_t *)ctx->impl;

    pthread_mutex_lock(&a->q_mu);
    a->stopping = 1;
    pthread_cond_broadcast(&a->q_cv);
    pthread_mutex_unlock(&a->q_mu);

    for (i = 0; i < a->nthreads; i++)
        pthread_join(a->threads[i], NULL);

    while (a->q_head) {
        krt_aix_job_t *j = a->q_head;
        a->q_head = j->next;
        free(j);
    }

    if (a->cq_shm.base)
        perl_krt_shm_destroy(&a->cq_shm);
    if (a->notify_shm.base)
        perl_krt_shm_destroy(&a->notify_shm);
    krt_reqtab_free(&a->reqs);
    free(a->files);
#if KRT_AIX_HAS_POLLSET
    if (a->ps != (pollset_t)-1)
        pollset_destroy(a->ps);
#endif
    if (a->wake_pipe[0] >= 0)
        close(a->wake_pipe[0]);
    if (a->wake_pipe[1] >= 0)
        close(a->wake_pipe[1]);
    pthread_cond_destroy(&a->q_cv);
    pthread_mutex_destroy(&a->q_mu);
    free(a);
    memset(ctx, 0, sizeof *ctx);
    return 0;
}

#else
const int perl_krt_aix_backend = 0; /* keeps the object non-empty */
#endif /* _AIX */

/*    krt_linux.c
 *
 *    perl5-mt kernel runtime adapter, Linux backend (amd64, ppc64le):
 *      shm        memfd_create + F_ADD_SEALS, mprotect
 *      comp       fork / clone(CLONE_FILES|CLONE_FS|SIGCHLD) without CLONE_VM
 *      wait/wake  futex
 *      cpu id     rseq (glibc >= 2.35 registration), sched_getcpu fallback
 *      aio        io_uring via raw syscalls (no liburing)
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#if defined(__linux__)

#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/futex.h>
#include <linux/io_uring.h>
#include <linux/stat.h>

#include "krt.h"
#include "krt_internal.h"
#include "krt_wait_tier.h"

/* ---- misc --------------------------------------------------------------- */

uint64_t
perl_krt_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return krt_ts_to_ns(&ts);
}

size_t
perl_krt_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
}

const char *
perl_krt_platform(void)
{
#if defined(__powerpc64__)
    return "linux-ppc64le/io_uring";
#elif defined(__x86_64__)
    return "linux-amd64/io_uring";
#else
    return "linux/io_uring";
#endif
}

static size_t
krt_round_page(size_t n)
{
    size_t p = perl_krt_page_size();
    return (n + p - 1) & ~(p - 1);
}

/* The descriptors krt holds for the life of the process (the arena memfd
 * and its read-only alias) are moved above the range scripts expect their
 * own files to land in, so that a program that assumes its first open()
 * gets fd 3 behaves as under stock perl.  CLOEXEC stays: compartments get
 * the descriptor only through the explicit SHARE_FILES path.           */
static int
krt_high_fd(int fd)
{
    int hi = fcntl(fd, F_DUPFD_CLOEXEC, 1000);
    if (hi < 0)
        return fd;                 /* keep the low one rather than fail */
    close(fd);
    return hi;
}

/* ---- 1. shm: memfd ------------------------------------------------------ */

int
perl_krt_shm_create(perl_krt_shm_t *out, size_t size, uint32_t flags)
{
    int fd;
    void *base;

    if (!out || size == 0)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->fd = out->ro_fd = -1;
    size = krt_round_page(size);

    fd = memfd_create("perl_krt_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return KRT_NEG_ERRNO();
    fd = krt_high_fd(fd);
    if (ftruncate(fd, (off_t)size) < 0) {
        int e = KRT_NEG_ERRNO();
        close(fd);
        return e;
    }
    base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        int e = KRT_NEG_ERRNO();
        close(fd);
        return e;
    }
    if (flags & PERL_KRT_SHM_NOINHERIT)
        (void)madvise(base, size, MADV_DONTFORK);

    out->fd    = fd;
    out->base  = base;
    out->len   = size;
    out->flags = flags & ~PERL_KRT_SHM_SEALED;
    return 0;
}

/*
 * memfd write seals (F_SEAL_WRITE) are refused while any mapping created
 * from a read-write descriptor exists, even after mprotect(PROT_READ),
 * because such mappings still carry VM_MAYWRITE.  So when sealing the
 * whole segment we open a read-only alias of the memfd, replace our
 * mapping in place (MAP_FIXED at the same address, same pages) with one
 * made from that alias, and only then add the seal.  The base address is
 * unchanged, which is what the frozen boot image relies on.
 */
int
perl_krt_shm_seal_ro(perl_krt_shm_t *shm, void *addr, size_t len)
{
    char path[64];
    int ro_fd;

    if (!shm || shm->fd < 0 || !addr || len == 0)
        return -EINVAL;
    if ((char *)addr < (char *)shm->base
        || (char *)addr + len > (char *)shm->base + shm->len)
        return -ERANGE;

    if (addr != shm->base || len != shm->len) {
        /* partial: protection only */
        if (mprotect(addr, len, PROT_READ) < 0)
            return KRT_NEG_ERRNO();
        return 0;
    }
    if (shm->flags & PERL_KRT_SHM_SEALED)
        return 0;

    snprintf(path, sizeof path, "/proc/self/fd/%d", shm->fd);
    ro_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (ro_fd < 0)
        return KRT_NEG_ERRNO();
    ro_fd = krt_high_fd(ro_fd);

    if (mmap(shm->base, shm->len, PROT_READ, MAP_SHARED | MAP_FIXED, ro_fd, 0)
        == MAP_FAILED) {
        int e = KRT_NEG_ERRNO();
        close(ro_fd);
        return e;
    }
    /* F_SEAL_WRITE needs every writable mapping gone, including ones a
     * compartment inherited; callers seal before spawning compartments. */
    if (fcntl(shm->fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        int e = KRT_NEG_ERRNO();
        if (e == -EBUSY) {
            /* best effort: forbid new writable mappings at least */
            (void)fcntl(shm->fd, F_ADD_SEALS,
                        F_SEAL_FUTURE_WRITE | F_SEAL_SHRINK | F_SEAL_GROW);
        }
        else {
            close(ro_fd);
            return e;
        }
    }
    shm->ro_fd  = ro_fd;
    shm->flags |= PERL_KRT_SHM_SEALED;
    return 0;
}

int
perl_krt_shm_remap(const perl_krt_shm_t *shm, void **addr_out, int prot)
{
    int fd;
    void *p;

    if (!shm || !addr_out || shm->fd < 0)
        return -EINVAL;
    if ((shm->flags & PERL_KRT_SHM_SEALED) && (prot & PROT_WRITE))
        return -EPERM;
    fd = (shm->flags & PERL_KRT_SHM_SEALED) && shm->ro_fd >= 0 ? shm->ro_fd
                                                               : shm->fd;
    p = mmap(NULL, shm->len, prot, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return KRT_NEG_ERRNO();
    *addr_out = p;
    return 0;
}

int
perl_krt_shm_unmap(void *addr, size_t len)
{
    if (!addr || len == 0)
        return -EINVAL;
    return munmap(addr, len) < 0 ? KRT_NEG_ERRNO() : 0;
}

int
perl_krt_shm_destroy(perl_krt_shm_t *shm)
{
    if (!shm)
        return -EINVAL;
    if (shm->base && shm->len)
        munmap(shm->base, shm->len);
    if (shm->ro_fd >= 0)
        close(shm->ro_fd);
    if (shm->fd >= 0)
        close(shm->fd);
    memset(shm, 0, sizeof *shm);
    shm->fd = shm->ro_fd = -1;
    return 0;
}

int
perl_krt_shm_seal_size(perl_krt_shm_t *shm)
{
    char path[64];

    if (!shm || shm->fd < 0)
        return -EINVAL;
    if (shm->ro_fd < 0) {
        snprintf(path, sizeof path, "/proc/self/fd/%d", shm->fd);
        shm->ro_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (shm->ro_fd < 0)
            return KRT_NEG_ERRNO();
        shm->ro_fd = krt_high_fd(shm->ro_fd);
    }
    if (fcntl(shm->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) < 0) {
        int e = KRT_NEG_ERRNO();
        if (e != -EPERM)       /* already sealed with F_SEAL_SEAL */
            return e;
    }
    return 0;
}

/* ---- 1b. protection keys: pkey_* via glibc ------------------------------ */

int
perl_krt_key_alloc(perl_krt_key_t *out)
{
    int k;
    if (!out)
        return -EINVAL;
    *out = PERL_KRT_KEY_NONE;
    k = pkey_alloc(0, 0);
    if (k < 0)
        return errno == ENOSYS || errno == EINVAL ? PERL_KRT_ENOSYS
                                                  : KRT_NEG_ERRNO();
    *out = k;
    return 0;
}

int
perl_krt_key_free(perl_krt_key_t key)
{
    if (key < 0)
        return -EINVAL;
    return pkey_free(key) < 0 ? KRT_NEG_ERRNO() : 0;
}

int
perl_krt_key_arm(perl_krt_key_t key, uint32_t flags)
{
    unsigned int rights = 0;
    if (key < 0)
        return -EINVAL;
    if (flags & PERL_KRT_KEY_NOACCESS)
        rights = PKEY_DISABLE_ACCESS;
    else if (flags & PERL_KRT_KEY_NOWRITE)
        rights = PKEY_DISABLE_WRITE;
    return pkey_set(key, rights) < 0 ? KRT_NEG_ERRNO() : 0;
}

int
perl_krt_shm_bind_key(perl_krt_shm_t *shm, perl_krt_key_t key)
{
    if (!shm || !shm->base || key < 0)
        return -EINVAL;
    if (pkey_mprotect(shm->base, shm->len, PROT_READ | PROT_WRITE, key) < 0)
        return errno == ENOSYS ? PERL_KRT_ENOSYS : KRT_NEG_ERRNO();
    return 0;
}

/* ---- 2. compartments: fork / clone(!CLONE_VM) --------------------------- */

static int krt_comp_child_ctl = -1;

int
perl_krt_comp_ctl_fd(void)
{
    return krt_comp_child_ctl;
}

static int
krt_comp_child(perl_krt_comp_fn fn, void *arg, int child_sock)
{
    int rc;
    sigset_t all;
    krt_comp_child_ctl = child_sock;
    /* leave the parent's handlers alone, but unblock everything */
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    rc = fn(arg);
    _exit(rc & 0xff);
}

int
perl_krt_comp_spawn_flags(perl_krt_comp_t *out, perl_krt_comp_fn fn, void *arg,
                          const perl_krt_shm_t *segs, size_t n_segs,
                          uint32_t flags)
{
    int sv[2];
    pid_t pid;

    (void)segs; (void)n_segs;   /* MAP_SHARED mappings are inherited */
    if (!out || !fn)
        return -EINVAL;
    memset(out, 0, sizeof *out);
    out->ctl_fd = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return KRT_NEG_ERRNO();

    if (flags & PERL_KRT_COMP_SHARE_FILES) {
        /* CLONE_SIGHAND is not allowed without CLONE_VM; the child keeps a
         * copy of the handler table instead. */
        long r = syscall(SYS_clone,
                         (unsigned long)(CLONE_FILES | CLONE_FS | SIGCHLD),
                         (void *)0, (void *)0, (void *)0, (void *)0);
        if (r < 0) {
            int e = KRT_NEG_ERRNO();
            close(sv[0]); close(sv[1]);
            return e;
        }
        pid = (pid_t)r;
        if (pid == 0) {
            /* shared table: do not close the parent's end here */
            krt_comp_child(fn, arg, sv[1]);
        }
    }
    else {
        pid = fork();
        if (pid < 0) {
            int e = KRT_NEG_ERRNO();
            close(sv[0]); close(sv[1]);
            return e;
        }
        if (pid == 0) {
            close(sv[0]);
            krt_comp_child(fn, arg, sv[1]);
        }
    }
    if (!(flags & PERL_KRT_COMP_SHARE_FILES))
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

/* ---- 3. wait / wake: futex ---------------------------------------------- */

void
perl_krt_wait_u32_flags(volatile uint32_t *seq, uint32_t expected,
                        uint64_t timeout_ns, uint32_t flags)
{
    struct timespec ts, *tsp = NULL;
    const int op = (flags & PERL_KRT_WAIT_SHARED) ? FUTEX_WAIT
                                                  : FUTEX_WAIT_PRIVATE;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER) {
        krt_ns_to_ts(timeout_ns, &ts);
        tsp = &ts;
    }
    (void)syscall(SYS_futex, seq, op, expected, tsp, NULL, 0);
}

void
perl_krt_wake_u32_flags(volatile uint32_t *seq, uint32_t count, uint32_t flags)
{
    const int op = (flags & PERL_KRT_WAIT_SHARED) ? FUTEX_WAKE
                                                  : FUTEX_WAKE_PRIVATE;
    (void)syscall(SYS_futex, seq, op,
                  count == UINT32_MAX ? INT_MAX : (int)count, NULL, NULL, 0);
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

/* ---- 3b. short-spin wait ------------------------------------------------
 * The tier code is CPU-dependent, not OS-dependent, so it lives in
 * krt_wait_tier.h and is shared with krt_darwin.c.                        */

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

/* ---- 4. cpu id: rseq ---------------------------------------------------- */

/* glibc >= 2.35 registers rseq for every thread and exports the TLS
 * offset of the rseq area; cpu_id lives at offset 4 in struct rseq.  */
extern const ptrdiff_t __rseq_offset __attribute__((weak));
extern const unsigned int __rseq_size __attribute__((weak));

int
perl_krt_cpu_id(void)
{
    if (&__rseq_offset && &__rseq_size && __rseq_size >= 8) {
        const char *tp = (const char *)perl_krt_thread_pointer();
        if (tp) {
            const volatile uint32_t *cpu_id =
                (const volatile uint32_t *)(tp + __rseq_offset + 4);
            int32_t c = (int32_t)*cpu_id;
            if (c >= 0)
                return c;
        }
    }
    {
        int c = sched_getcpu();
        return c < 0 ? -1 : c;
    }
}

int
perl_krt_cpu_count(void)
{
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return n;
    }
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? (int)n : 1;
    }
}

/* ---- 4b. affinity: pinning, SMT siblings, NUMA node ----------------------- */

int
perl_krt_cpu_pin(int cpu)
{
    cpu_set_t set;
    if (cpu < 0 || cpu >= CPU_SETSIZE)
        return -EINVAL;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    /* pid 0 is the calling thread on Linux                                */
    return sched_setaffinity(0, sizeof set, &set) == 0 ? 0 : KRT_NEG_ERRNO();
}

/* Parse a sysfs cpu list ("0,4", "0-3", "0-1,8-9\n") into out, skipping
 * self.  Returns the number of entries written (at most max).             */
static int
krt_parse_cpu_list(const char *s, int self, int *out, int max)
{
    int n = 0;
    while (*s && *s != '\n') {
        char *end;
        long lo = strtol(s, &end, 10), hi = lo, c;
        if (end == s)
            break;
        s = end;
        if (*s == '-') {
            hi = strtol(s + 1, &end, 10);
            s = end;
        }
        for (c = lo; c <= hi; c++) {
            if (c == self)
                continue;
            if (n >= max)
                return n;
            out[n++] = (int)c;
        }
        if (*s != ',')
            break;
        s++;
    }
    return n;
}

int
perl_krt_smt_siblings(int cpu, int *out, int max)
{
    char path[128], buf[256];
    FILE *f;
    size_t len;
    if (cpu < 0 || max < 0 || (max > 0 && !out))
        return -EINVAL;
    snprintf(path, sizeof path,
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    f = fopen(path, "r");
    if (!f)
        return KRT_NEG_ERRNO();
    len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    return krt_parse_cpu_list(buf, cpu, out, max);
}

int
perl_krt_numa_node(int cpu)
{
    char path[128];
    DIR *d;
    struct dirent *e;
    int node = -1;
    if (cpu < 0)
        return -1;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu);
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "node", 4) == 0
            && e->d_name[4] >= '0' && e->d_name[4] <= '9') {
            node = atoi(e->d_name + 4);
            break;
        }
    }
    closedir(d);
    return node;
}

/* ---- 5. aio: io_uring --------------------------------------------------- */

#define KRT_UD_EVENTFD      ((uint64_t)-1)       /* internal sentinel      */
#define KRT_UD_FUTEX_WAIT   ((uint64_t)-2)       /* futex wait sqe         */
#define KRT_UD_FUTEX_CANCEL ((uint64_t)-3)       /* futex cancel sqe       */
#ifndef IORING_OP_ASYNC_CANCEL
#  define IORING_OP_ASYNC_CANCEL 14
#endif
#ifndef IORING_OP_FUTEX_WAIT
#  define IORING_OP_FUTEX_WAIT   51
#endif
#ifndef FUTEX2_SIZE_U32
#  define FUTEX2_SIZE_U32        0x02u
#endif
#ifndef FUTEX2_PRIVATE
#  define FUTEX2_PRIVATE         128u
#endif
#ifndef FUTEX_BITSET_MATCH_ANY
#  define FUTEX_BITSET_MATCH_ANY 0xffffffffu
#endif
#define KRT_REQ_SENDZC_WAIT_NOTIF  1
#define KRT_REQ_DEFERRED_ERR       2
#define KRT_REQ_SPLICE_STAGE1      3   /* internal: file -> pipe half     */
#define KRT_REQ_SPLICE_STAGE2      4   /* user visible: pipe -> target    */

typedef struct krt_splice_pipe {
    int      p[2];
    uint32_t user_idx;          /* record that owns the user cqe          */
} krt_splice_pipe_t;

static int
krt_fd_is_pipe(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

typedef struct krt_uring {
    int              ring_fd;
    int              efd;                /* eventfd for notify wakeups    */
    uint32_t         features;
    uint32_t         setup_flags;
    /* SQ */
    void            *sq_ring;  size_t sq_ring_sz;
    perl_krt_au32_t *sq_head, *sq_tail, *sq_flags;
    uint32_t        *sq_mask, *sq_entries, *sq_array;
    struct io_uring_sqe *sqes;
    uint32_t         sq_local_tail;
    /* CQ */
    void            *cq_ring;  size_t cq_ring_sz;
    perl_krt_au32_t *cq_head, *cq_tail;
    uint32_t        *cq_mask, *cq_entries;
    struct io_uring_cqe *cqes;
    /* requests */
    krt_reqtab_t     reqs;
    struct statx    *statx_side;         /* one per request slot          */
    /* notify */
    perl_krt_shm_t   notify_shm;
    perl_krt_mpsc_t *notify_ring;
    uint64_t         efd_buf;
    int              efd_armed;
    int              buf_registered;
    int              futex_wait_enosys;  /* 1 when kernel refuses FUTEX_WAIT */
    int              futex_armed;        /* 1 when KRT_UD_FUTEX_WAIT in ring */
    /* pending cqes already converted but not yet returned */
    perl_krt_cqe_t  *stash; uint32_t stash_n, stash_cap;
} krt_uring_t;

static int
sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(SYS_io_uring_setup, entries, p);
}

static int
sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                   unsigned flags, const void *arg, size_t argsz)
{
    return (int)syscall(SYS_io_uring_enter, fd, to_submit, min_complete,
                        flags, arg, argsz);
}

static int
sys_io_uring_register(int fd, unsigned opcode, const void *arg, unsigned n)
{
    return (int)syscall(SYS_io_uring_register, fd, opcode, arg, n);
}

static struct io_uring_sqe *
krt_uring_get_sqe(krt_uring_t *u)
{
    uint32_t head = perl_krt_load_acquire(u->sq_head);
    uint32_t next = u->sq_local_tail + 1;
    if (next - head > *u->sq_entries)
        return NULL;
    {
        uint32_t idx = u->sq_local_tail & *u->sq_mask;
        struct io_uring_sqe *s = &u->sqes[idx];
        memset(s, 0, sizeof *s);
        u->sq_array[idx] = idx;
        u->sq_local_tail = next;
        return s;
    }
}

static int
krt_uring_flush_sq(krt_uring_t *u)
{
    uint32_t ktail = perl_krt_load_relaxed(u->sq_tail);
    uint32_t to_submit = u->sq_local_tail - ktail;
    if (to_submit == 0)
        return 0;
    perl_krt_store_release(u->sq_tail, u->sq_local_tail);
    if (u->setup_flags & IORING_SETUP_SQPOLL) {
        if (perl_krt_load_seq_cst(u->sq_flags) & IORING_SQ_NEED_WAKEUP) {
            if (sys_io_uring_enter(u->ring_fd, to_submit, 0,
                                   IORING_ENTER_SQ_WAKEUP, NULL, 0) < 0
                && errno != EINTR)
                return KRT_NEG_ERRNO();
        }
        return (int)to_submit;
    }
    {
        int r;
        do {
            r = sys_io_uring_enter(u->ring_fd, to_submit, 0, 0, NULL, 0);
        } while (r < 0 && errno == EINTR);
        return r < 0 ? KRT_NEG_ERRNO() : r;
    }
}

static int
krt_uring_arm_eventfd(krt_uring_t *u)
{
    struct io_uring_sqe *s;
    if (u->efd_armed)
        return 0;
    s = krt_uring_get_sqe(u);
    if (!s)
        return -EBUSY;
    s->opcode    = IORING_OP_READ;
    s->fd        = u->efd;
    s->addr      = (uint64_t)(uintptr_t)&u->efd_buf;
    s->len       = sizeof u->efd_buf;
    s->off       = 0;
    s->user_data = KRT_UD_EVENTFD;
    u->efd_armed = 1;
    return 0;
}

int
perl_krt_aio_init_flags(perl_krt_aio_t *ctx, uint32_t depth,
                        perl_krt_shm_t *reg_buf, uint32_t flags)
{
    struct io_uring_params p;
    krt_uring_t *u;
    int rc, fd;
    size_t sz;

    if (!ctx || depth < 2 || depth > 32768)
        return -EINVAL;
    memset(ctx, 0, sizeof *ctx);
    u = (krt_uring_t *)calloc(1, sizeof *u);
    if (!u)
        return -ENOMEM;
    u->ring_fd = u->efd = -1;
    u->notify_shm.fd = u->notify_shm.ro_fd = -1;

    memset(&p, 0, sizeof p);
    if (flags & PERL_KRT_AIO_SQPOLL) {
        p.flags |= IORING_SETUP_SQPOLL;
        p.sq_thread_idle = 2000;
    }
    p.flags |= IORING_SETUP_CLAMP;
    fd = sys_io_uring_setup(depth, &p);
    if (fd < 0 && (p.flags & IORING_SETUP_SQPOLL)) {
        /* SQPOLL may need privileges; retry without */
        p.flags &= ~IORING_SETUP_SQPOLL;
        fd = sys_io_uring_setup(depth, &p);
    }
    if (fd < 0) {
        rc = KRT_NEG_ERRNO();
        free(u);
        return rc == -ENOSYS ? PERL_KRT_ENOSYS : rc;
    }
    u->ring_fd     = fd;
    u->features    = p.features;
    u->setup_flags = p.flags;

    u->sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    u->cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (u->cq_ring_sz > u->sq_ring_sz) u->sq_ring_sz = u->cq_ring_sz;
        u->cq_ring_sz = u->sq_ring_sz;
    }
    u->sq_ring = mmap(NULL, u->sq_ring_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (u->sq_ring == MAP_FAILED) { rc = KRT_NEG_ERRNO(); goto fail; }
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        u->cq_ring = u->sq_ring;
    }
    else {
        u->cq_ring = mmap(NULL, u->cq_ring_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
        if (u->cq_ring == MAP_FAILED) { u->cq_ring = NULL; rc = KRT_NEG_ERRNO(); goto fail; }
    }
    sz = p.sq_entries * sizeof(struct io_uring_sqe);
    u->sqes = (struct io_uring_sqe *)mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED) { u->sqes = NULL; rc = KRT_NEG_ERRNO(); goto fail; }

#define SQ_U32(off)  ((perl_krt_au32_t *)((char *)u->sq_ring + (off)))
#define SQ_PLAIN(off) ((uint32_t *)((char *)u->sq_ring + (off)))
#define CQ_U32(off)  ((perl_krt_au32_t *)((char *)u->cq_ring + (off)))
#define CQ_PLAIN(off) ((uint32_t *)((char *)u->cq_ring + (off)))
    u->sq_head    = SQ_U32(p.sq_off.head);
    u->sq_tail    = SQ_U32(p.sq_off.tail);
    u->sq_mask    = SQ_PLAIN(p.sq_off.ring_mask);
    u->sq_entries = SQ_PLAIN(p.sq_off.ring_entries);
    u->sq_flags   = SQ_U32(p.sq_off.flags);
    u->sq_array   = SQ_PLAIN(p.sq_off.array);
    u->cq_head    = CQ_U32(p.cq_off.head);
    u->cq_tail    = CQ_U32(p.cq_off.tail);
    u->cq_mask    = CQ_PLAIN(p.cq_off.ring_mask);
    u->cq_entries = CQ_PLAIN(p.cq_off.ring_entries);
    u->cqes       = (struct io_uring_cqe *)((char *)u->cq_ring + p.cq_off.cqes);
#undef SQ_U32
#undef SQ_PLAIN
#undef CQ_U32
#undef CQ_PLAIN
    u->sq_local_tail = perl_krt_load_relaxed(u->sq_tail);

    rc = krt_reqtab_init(&u->reqs, p.sq_entries);
    if (rc) goto fail;
    u->statx_side = (struct statx *)calloc(p.sq_entries, sizeof(struct statx));
    if (!u->statx_side) { rc = -ENOMEM; goto fail; }
    u->stash_cap = p.cq_entries;
    u->stash = (perl_krt_cqe_t *)calloc(u->stash_cap, sizeof(perl_krt_cqe_t));
    if (!u->stash) { rc = -ENOMEM; goto fail; }

    /* notify path: eventfd + MPSC ring in (inheritable) shared memory */
    u->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (u->efd < 0) { rc = KRT_NEG_ERRNO(); goto fail; }
    rc = perl_krt_shm_create(&u->notify_shm,
                             perl_krt_mpsc_bytes(KRT_NOTIFY_RING_CAP, 8), 0);
    if (rc) goto fail;
    u->notify_ring = perl_krt_mpsc_init(u->notify_shm.base, u->notify_shm.len,
                                        KRT_NOTIFY_RING_CAP, 8);
    if (!u->notify_ring) { rc = -EINVAL; goto fail; }

    if (reg_buf && reg_buf->base && reg_buf->len) {
        struct iovec iov = { reg_buf->base, reg_buf->len };
        if (sys_io_uring_register(fd, IORING_REGISTER_BUFFERS, &iov, 1) == 0)
            u->buf_registered = 1;
        else if (errno != ENOMEM && errno != EPERM) {
            rc = KRT_NEG_ERRNO(); goto fail;
        }
        /* registered buffers are charged to RLIMIT_MEMLOCK, so
         * with many rings (one per worker) the registration can run out of
         * locked memory.  The ring then runs without them: the fixed-buffer
         * operations are refused and the caller uses private buffers.     */
    }

    ctx->depth   = p.sq_entries;
    ctx->flags   = (p.flags & IORING_SETUP_SQPOLL) ? PERL_KRT_AIO_SQPOLL : 0;
    ctx->reg_buf = reg_buf;
    ctx->impl    = u;

    rc = krt_uring_arm_eventfd(u);
    if (rc) goto fail_ctx;
    rc = krt_uring_flush_sq(u);
    if (rc < 0) goto fail_ctx;
    return 0;

fail_ctx:
    ctx->impl = NULL;
fail:
    if (u->sqes) munmap(u->sqes, p.sq_entries * sizeof(struct io_uring_sqe));
    if (u->cq_ring && u->cq_ring != u->sq_ring) munmap(u->cq_ring, u->cq_ring_sz);
    if (u->sq_ring && u->sq_ring != MAP_FAILED) munmap(u->sq_ring, u->sq_ring_sz);
    if (u->notify_shm.fd >= 0) perl_krt_shm_destroy(&u->notify_shm);
    if (u->efd >= 0) close(u->efd);
    if (u->ring_fd >= 0) close(u->ring_fd);
    krt_reqtab_free(&u->reqs);
    free(u->statx_side);
    free(u->stash);
    free(u);
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
    krt_uring_t *u;
    if (!ctx || !ctx->impl || !fds || n == 0)
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    (void)sys_io_uring_register(u->ring_fd, IORING_UNREGISTER_FILES, NULL, 0);
    if (sys_io_uring_register(u->ring_fd, IORING_REGISTER_FILES, fds, n) < 0)
        return KRT_NEG_ERRNO();
    return 0;
}

int
perl_krt_aio_wakeup_fd(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl) ? ((krt_uring_t *)ctx->impl)->ring_fd : -1;
}

static int
krt_uring_fill(krt_uring_t *u, perl_krt_aio_t *ctx, const perl_krt_sqe_t *q,
               struct io_uring_sqe *s, krt_req_t *r)
{
    uint64_t addr = q->addr;
    if (q->flags & PERL_KRT_SQE_FIXED_BUF) {
        if (!u->buf_registered || !ctx->reg_buf
            || q->addr + q->len > ctx->reg_buf->len)
            return -EINVAL;
        addr = (uint64_t)(uintptr_t)ctx->reg_buf->base + q->addr;
    }
    s->fd = q->fd;
    if (q->flags & PERL_KRT_SQE_FIXED_FILE)
        s->flags |= IOSQE_FIXED_FILE;

    switch (q->op) {
    case PERL_KRT_AIO_NOP:
        s->opcode = IORING_OP_NOP;
        break;
    case PERL_KRT_AIO_OPENAT:
        s->opcode     = IORING_OP_OPENAT;
        s->addr       = q->addr;            /* path, never fixed buf */
        s->open_flags = q->open_flags | O_CLOEXEC;
        s->len        = q->mode;
        break;
    case PERL_KRT_AIO_STATX:
        if (!q->addr2)
            return -EINVAL;
        s->opcode      = IORING_OP_STATX;
        s->addr        = q->addr ? q->addr : (uint64_t)(uintptr_t)"";
        s->statx_flags = q->open_flags;     /* AT_* flags */
        s->len         = STATX_BASIC_STATS | STATX_BTIME;
        r->dst  = (void *)(uintptr_t)q->addr2;
        r->side = &u->statx_side[(size_t)(r - u->reqs.reqs)];
        s->off  = (uint64_t)(uintptr_t)r->side;   /* addr2 */
        break;
    case PERL_KRT_AIO_READ:
        s->opcode = (q->flags & PERL_KRT_SQE_FIXED_BUF) ? IORING_OP_READ_FIXED
                                                        : IORING_OP_READ;
        s->addr = addr; s->len = q->len; s->off = q->off;
        s->buf_index = 0;
        break;
    case PERL_KRT_AIO_WRITE:
        s->opcode = (q->flags & PERL_KRT_SQE_FIXED_BUF) ? IORING_OP_WRITE_FIXED
                                                        : IORING_OP_WRITE;
        s->addr = addr; s->len = q->len; s->off = q->off;
        s->buf_index = 0;
        break;
    case PERL_KRT_AIO_FSYNC:
        s->opcode = IORING_OP_FSYNC;
        s->fsync_flags = 0;
        break;
    case PERL_KRT_AIO_SPLICE:
        s->flags &= ~IOSQE_FIXED_FILE;
        if (krt_fd_is_pipe(q->fd) || krt_fd_is_pipe(q->fd2)) {
            s->opcode        = IORING_OP_SPLICE;
            s->splice_fd_in  = q->fd;
            s->splice_off_in = q->off;
            s->fd            = q->fd2;
            s->off           = q->off2;
            s->len           = q->len;
            s->splice_flags  = 0;
            break;
        }
        /* neither side is a pipe: stage through a private pipe with two
         * linked sqes; `s` becomes stage 1 and the caller's record moves
         * to a second sqe (see perl_krt_aio_submit)                      */
        return 1;

    case PERL_KRT_AIO_SEND_ZC: {
        /* zero-copy send exists for INET sockets only; everything else
         * (AF_UNIX, pipes) gets an ordinary SEND with the same semantics */
        int domain = AF_UNSPEC, zc = 0;
        socklen_t dl = sizeof domain;
        if (!(q->flags & PERL_KRT_SQE_FIXED_FILE)
            && getsockopt(q->fd, SOL_SOCKET, SO_DOMAIN, &domain, &dl) == 0
            && (domain == AF_INET || domain == AF_INET6))
            zc = 1;
        s->opcode    = zc ? IORING_OP_SEND_ZC : IORING_OP_SEND;
        s->addr      = addr; s->len = q->len;
        s->msg_flags = MSG_NOSIGNAL;
        if (zc && (q->flags & PERL_KRT_SQE_FIXED_BUF)) {
            s->ioprio   |= IORING_RECVSEND_FIXED_BUF;
            s->buf_index = 0;
        }
        r->op    = zc ? PERL_KRT_AIO_SEND_ZC : PERL_KRT_AIO_WRITE;
        r->state = zc ? KRT_REQ_SENDZC_WAIT_NOTIF : 0;
        break;
    }
    case PERL_KRT_AIO_CLOSE:
        s->opcode = IORING_OP_CLOSE;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int
perl_krt_aio_submit(perl_krt_aio_t *ctx, const perl_krt_sqe_t *sqes, uint32_t n)
{
    krt_uring_t *u;
    uint32_t i;
    int rc;

    if (!ctx || !ctx->impl || (!sqes && n))
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    for (i = 0; i < n; i++) {
        struct io_uring_sqe *s;
        krt_req_t *r;
        uint32_t idx;
        /* keep one SQ slot for the eventfd re-arm */
        if (u->sq_local_tail + 1 - perl_krt_load_acquire(u->sq_head)
            >= *u->sq_entries)
            break;
        r = krt_req_alloc(&u->reqs, &idx);
        if (!r)
            break;
        s = krt_uring_get_sqe(u);
        if (!s) {
            krt_req_release(&u->reqs, idx);
            break;
        }
        r->user_data = sqes[i].user_data;
        r->op        = sqes[i].op;
        rc = krt_uring_fill(u, ctx, &sqes[i], s, r);
        if (rc == 1) {
            /* two-step splice: s = stage 1 (in -> pipe), linked to a
             * second sqe (pipe -> out) that carries the user's record */
            krt_splice_pipe_t *sp;
            krt_req_t *r1;
            uint32_t idx1;
            struct io_uring_sqe *s2;
            int psz;
            if (u->sq_local_tail + 2 - perl_krt_load_acquire(u->sq_head)
                    > *u->sq_entries
                || !(r1 = krt_req_alloc(&u->reqs, &idx1))) {
                rc = -EBUSY;
                goto splice_err;
            }
            sp = (krt_splice_pipe_t *)calloc(1, sizeof *sp);
            if (!sp || pipe2(sp->p, O_CLOEXEC | O_NONBLOCK) < 0) {
                free(sp);
                krt_req_release(&u->reqs, idx1);
                rc = -ENOMEM;
                goto splice_err;
            }
            psz = fcntl(sp->p[0], F_SETPIPE_SZ, 1 << 20);
            if (psz < 0) psz = fcntl(sp->p[0], F_GETPIPE_SZ);
            if (psz > 0 && sqes[i].len > (uint32_t)psz) {
                close(sp->p[0]); close(sp->p[1]); free(sp);
                krt_req_release(&u->reqs, idx1);
                rc = -E2BIG;
                goto splice_err;
            }
            sp->user_idx = idx;
            r1->op = PERL_KRT_AIO_SPLICE; r1->state = KRT_REQ_SPLICE_STAGE1;
            r1->side = sp;
            r->state = KRT_REQ_SPLICE_STAGE2; r->side = sp;

            s->opcode        = IORING_OP_SPLICE;
            s->splice_fd_in  = sqes[i].fd;
            s->splice_off_in = sqes[i].off;
            s->fd            = sp->p[1];
            s->off           = (uint64_t)-1;
            s->len           = sqes[i].len;
            s->splice_flags  = 0;
            s->flags        |= IOSQE_IO_LINK;
            s->user_data     = idx1;

            s2 = krt_uring_get_sqe(u);
            s2->opcode        = IORING_OP_SPLICE;
            s2->splice_fd_in  = sp->p[0];
            s2->splice_off_in = (uint64_t)-1;
            s2->fd            = sqes[i].fd2;
            s2->off           = sqes[i].off2;
            s2->len           = sqes[i].len;
            s2->splice_flags  = 0;
            s2->user_data     = idx;
            continue;
        splice_err:
            memset(s, 0, sizeof *s);
            s->opcode = IORING_OP_NOP;
            r->res    = rc;
            r->state  = KRT_REQ_DEFERRED_ERR;
            r->side   = NULL;
        }
        else if (rc) {
            /* turn the slot into a NOP that completes with the error */
            memset(s, 0, sizeof *s);
            s->opcode = IORING_OP_NOP;
            r->res    = rc;
            r->state  = KRT_REQ_DEFERRED_ERR;
        }
        s->user_data = idx;
    }
    rc = krt_uring_flush_sq(u);
    if (rc < 0)
        return rc;
    return (int)i;
}

/* Convert one kernel cqe; returns 1 when a user cqe was produced. */
static int
krt_uring_convert(krt_uring_t *u, const struct io_uring_cqe *k,
                  perl_krt_cqe_t *out)
{
    krt_req_t *r;
    uint32_t idx;

    if (k->user_data == KRT_UD_EVENTFD) {
        u->efd_armed = 0;          /* drained the counter; re-arm later */
        return 0;
    }
    if (k->user_data == KRT_UD_FUTEX_WAIT) {
        u->futex_armed = 0;
        if (k->res == -EINVAL || k->res == -EOPNOTSUPP || k->res == -ENOSYS)
            u->futex_wait_enosys = 1;
        return 0;
    }
    if (k->user_data == KRT_UD_FUTEX_CANCEL) {
        return 0;
    }
    idx = (uint32_t)k->user_data;
    if (idx >= u->reqs.n)
        return 0;
    r = &u->reqs.reqs[idx];

    if (r->op == PERL_KRT_AIO_SEND_ZC) {
        if (k->flags & IORING_CQE_F_NOTIF) {
            /* buffer may be reused now: report the stored result */
            out->user_data = r->user_data;
            out->res       = r->res;
            out->flags     = 0;
            krt_req_release(&u->reqs, idx);
            return 1;
        }
        r->res = k->res;
        if (k->flags & IORING_CQE_F_MORE)
            return 0;              /* wait for the NOTIF cqe */
        out->user_data = r->user_data; out->res = r->res; out->flags = 0;
        krt_req_release(&u->reqs, idx);
        return 1;
    }
    if (r->state == KRT_REQ_SPLICE_STAGE1) {
        /* internal half: on failure hand the error to the user record */
        krt_splice_pipe_t *sp = (krt_splice_pipe_t *)r->side;
        if (k->res < 0) {
            krt_req_t *ur = &u->reqs.reqs[sp->user_idx];
            ur->res   = k->res;
            ur->state = KRT_REQ_DEFERRED_ERR;   /* stage 2 gets -ECANCELED */
        }
        krt_req_release(&u->reqs, idx);
        return 0;
    }
    out->user_data = r->user_data;
    out->flags     = 0;
    if (r->state == KRT_REQ_SPLICE_STAGE2) {
        krt_splice_pipe_t *sp = (krt_splice_pipe_t *)r->side;
        close(sp->p[0]); close(sp->p[1]); free(sp);
        out->res = k->res;
        krt_req_release(&u->reqs, idx);
        return 1;
    }
    if (r->state == KRT_REQ_DEFERRED_ERR) {
        if (r->side) {              /* failed two-step splice */
            krt_splice_pipe_t *sp = (krt_splice_pipe_t *)r->side;
            close(sp->p[0]); close(sp->p[1]); free(sp);
        }
        out->res = r->res;         /* deferred submit-time / stage-1 error */
    }
    else {
        out->res = k->res;
        if (r->op == PERL_KRT_AIO_STATX && k->res == 0 && r->dst && r->side) {
            const struct statx *sx = (const struct statx *)r->side;
            perl_krt_stat_t *d = (perl_krt_stat_t *)r->dst;
            d->dev   = (uint64_t)makedev(sx->stx_dev_major, sx->stx_dev_minor);
            d->ino   = sx->stx_ino;   d->nlink = sx->stx_nlink;
            d->mode  = sx->stx_mode;  d->uid = sx->stx_uid; d->gid = sx->stx_gid;
            d->rdev  = (uint64_t)makedev(sx->stx_rdev_major, sx->stx_rdev_minor);
            d->size  = sx->stx_size;  d->blksize = sx->stx_blksize;
            d->blocks = sx->stx_blocks;
            d->atime_ns = (int64_t)sx->stx_atime.tv_sec * 1000000000 + sx->stx_atime.tv_nsec;
            d->mtime_ns = (int64_t)sx->stx_mtime.tv_sec * 1000000000 + sx->stx_mtime.tv_nsec;
            d->ctime_ns = (int64_t)sx->stx_ctime.tv_sec * 1000000000 + sx->stx_ctime.tv_nsec;
            d->btime_ns = (sx->stx_mask & STATX_BTIME)
                ? (int64_t)sx->stx_btime.tv_sec * 1000000000 + sx->stx_btime.tv_nsec : 0;
        }
    }
    krt_req_release(&u->reqs, idx);
    return 1;
}

static uint32_t
krt_uring_reap(krt_uring_t *u, perl_krt_cqe_t *cqes, uint32_t max)
{
    uint32_t n = 0;
    uint32_t head = perl_krt_load_relaxed(u->cq_head);
    uint32_t tail = perl_krt_load_acquire(u->cq_tail);
    while (head != tail && n < max) {
        const struct io_uring_cqe *k = &u->cqes[head & *u->cq_mask];
        n += (uint32_t)krt_uring_convert(u, k, &cqes[n]);
        head++;
    }
    perl_krt_store_release(u->cq_head, head);
    return n;
}

int
perl_krt_aio_drain(perl_krt_aio_t *ctx, perl_krt_cqe_t *cqes, uint32_t max,
                   uint64_t timeout_ns)
{
    krt_uring_t *u;
    uint32_t n = 0;
    uint64_t deadline = 0;

    if (!ctx || !ctx->impl || !cqes || max == 0)
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    if (timeout_ns != PERL_KRT_WAIT_FOREVER && timeout_ns != 0)
        deadline = perl_krt_now_ns() + timeout_ns;

    for (;;) {
        /* 1. previously converted but unreturned */
        while (u->stash_n && n < max)
            cqes[n++] = u->stash[--u->stash_n];
        /* 2. kernel completions */
        if (n < max)
            n += krt_uring_reap(u, cqes + n, max - n);
        /* 3. notifications */
        if (n < max)
            n += (uint32_t)krt_notify_pop(u->notify_ring, cqes + n, max - n);
        /* re-arm the eventfd reader if its read completed */
        if (!u->efd_armed) {
            if (krt_uring_arm_eventfd(u) == 0) {
                int rc = krt_uring_flush_sq(u);
                if (rc < 0 && n == 0) return rc;
            }
        }
        if (n || timeout_ns == 0)
            return (int)n;

        /* 4. block for at least one completion; a wake that delivers
         * nothing (eventfd re-arm, EINTR) is spurious: keep waiting */
        {
            struct io_uring_getevents_arg arg;
            struct __kernel_timespec kts;
            const void *argp = NULL;
            size_t argsz = 0;
            unsigned flags = IORING_ENTER_GETEVENTS;
            int r;
            if (deadline) {
                uint64_t now = perl_krt_now_ns();
                uint64_t left = deadline > now ? deadline - now : 0;
                if (left == 0)
                    return 0;
                memset(&arg, 0, sizeof arg);
                kts.tv_sec  = (long long)(left / 1000000000ull);
                kts.tv_nsec = (long long)(left % 1000000000ull);
                arg.ts = (uint64_t)(uintptr_t)&kts;
                argp = &arg; argsz = sizeof arg;
                flags |= IORING_ENTER_EXT_ARG;
            }
            r = sys_io_uring_enter(u->ring_fd, 0, 1, flags, argp, argsz);
            if (r < 0 && errno != EINTR && errno != ETIME && errno != EAGAIN)
                return KRT_NEG_ERRNO();
        }
    }
}

/* may the ring use SQE_FIXED_BUF operations?  False when the
 * registration was refused for locked memory (see perl_krt_aio_init_flags). */
int
perl_krt_aio_fixed_bufs(const perl_krt_aio_t *ctx)
{
    return (ctx && ctx->impl) ? ((krt_uring_t *)ctx->impl)->buf_registered : 0;
}

/* is a completion waiting for perl_krt_aio_drain?  Reads the
 * ring's indices only: no system call. */
int
perl_krt_aio_ready(const perl_krt_aio_t *ctx)
{
    krt_uring_t *u;
    if (!ctx || !ctx->impl)
        return 0;
    u = (krt_uring_t *)ctx->impl;
    return u->stash_n != 0
        || perl_krt_load_relaxed(u->cq_head) != perl_krt_load_acquire(u->cq_tail)
        || perl_krt_mpsc_count(u->notify_ring) != 0;
}

/* block until a completion is ready or timeout_ns passes, without
 * taking it: io_uring_enter(GETEVENTS) waits for a cqe to exist and leaves
 * it in the ring for the next drain.  Returns 0, or -errno. */
int
perl_krt_aio_wait(perl_krt_aio_t *ctx, uint64_t timeout_ns)
{
    krt_uring_t *u;
    struct io_uring_getevents_arg arg;
    struct __kernel_timespec kts;
    uint64_t deadline;
    int r;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    if (perl_krt_aio_ready(ctx))
        return 0;
    u = (krt_uring_t *)ctx->impl;
    if (timeout_ns == 0)
        return 0;
    deadline = timeout_ns == PERL_KRT_WAIT_FOREVER ? 0
             : perl_krt_now_ns() + timeout_ns;
    memset(&arg, 0, sizeof arg);
    if (deadline) {
        kts.tv_sec  = (long long)(timeout_ns / 1000000000ull);
        kts.tv_nsec = (long long)(timeout_ns % 1000000000ull);
        arg.ts = (uint64_t)(uintptr_t)&kts;
    }
    do {
        r = sys_io_uring_enter(u->ring_fd, 0, 1,
                               IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                               &arg, sizeof arg);
    } while (r < 0 && errno == EINTR);
    if (r < 0 && errno != ETIME && errno != EAGAIN)
        return KRT_NEG_ERRNO();
    return 0;
}

/* combined sleep covering both an aio completion and a slot-vector
 * doorbell wake in one io_uring_enter call via IORING_OP_FUTEX_WAIT.
 * Returns 0 on wake/timeout, PERL_KRT_ENOSYS when the kernel refuses
 * IORING_OP_FUTEX_WAIT (or PERL_KRT_ENOSYS=1 is set in the environment),
 * or -errno on error. */
int
perl_krt_aio_wait_doorbell(perl_krt_aio_t *ctx, volatile uint32_t *doorbell,
                           uint32_t expected, uint32_t wait_flags,
                           uint64_t timeout_ns)
{
    krt_uring_t *u;
    struct io_uring_sqe *s;
    struct io_uring_getevents_arg arg;
    struct __kernel_timespec kts;
    uint32_t ktail, to_submit;
    int r;

    if (!ctx || !ctx->impl || !doorbell)
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    if (!u->futex_wait_enosys) {
        const char *env = getenv("PERL_KRT_ENOSYS");
        if (env && *env && *env != '0')
            u->futex_wait_enosys = 1;
    }
    if (u->futex_wait_enosys)
        return PERL_KRT_ENOSYS;
    if (perl_krt_aio_ready(ctx) || *doorbell != expected || timeout_ns == 0)
        return 0;

    if (!u->futex_armed) {
        s = krt_uring_get_sqe(u);
        if (!s)
            return -EAGAIN;
        s->opcode    = IORING_OP_FUTEX_WAIT;
        s->fd        = (int)((wait_flags & PERL_KRT_WAIT_SHARED)
                             ? FUTEX2_SIZE_U32
                             : (FUTEX2_SIZE_U32 | FUTEX2_PRIVATE));
        s->addr      = (uint64_t)(uintptr_t)doorbell;
        s->addr2     = (uint64_t)expected;
        s->addr3     = (uint64_t)FUTEX_BITSET_MATCH_ANY;
        s->user_data = KRT_UD_FUTEX_WAIT;
        u->futex_armed = 1;
    }

    ktail = perl_krt_load_relaxed(u->sq_tail);
    to_submit = u->sq_local_tail - ktail;
    perl_krt_store_release(u->sq_tail, u->sq_local_tail);

    memset(&arg, 0, sizeof arg);
    if (timeout_ns != PERL_KRT_WAIT_FOREVER) {
        kts.tv_sec  = (long long)(timeout_ns / 1000000000ull);
        kts.tv_nsec = (long long)(timeout_ns % 1000000000ull);
        arg.ts = (uint64_t)(uintptr_t)&kts;
    }
    do {
        r = sys_io_uring_enter(u->ring_fd, to_submit, 1,
                               IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                               &arg, sizeof arg);
        to_submit = 0;
    } while (r < 0 && errno == EINTR);

    /* Inspect any internal FUTEX_WAIT CQE at the head so an unsupported
     * opcode is detected immediately and reported as PERL_KRT_ENOSYS. */
    {
        uint32_t head = perl_krt_load_relaxed(u->cq_head);
        uint32_t tail = perl_krt_load_acquire(u->cq_tail);
        while (head != tail) {
            struct io_uring_cqe *k = &u->cqes[head & *u->cq_mask];
            if (k->user_data == KRT_UD_FUTEX_WAIT) {
                u->futex_armed = 0;
                if (k->res == -EINVAL || k->res == -EOPNOTSUPP
                    || k->res == -ENOSYS)
                    u->futex_wait_enosys = 1;
                head++;
                perl_krt_store_release(u->cq_head, head);
                continue;
            }
            if (k->user_data == KRT_UD_FUTEX_CANCEL) {
                head++;
                perl_krt_store_release(u->cq_head, head);
                continue;
            }
            break;
        }
    }

    /* If we woke for an AIO completion or timeout while FUTEX_WAIT is still
     * armed in the kernel, cancel it so it does not linger. */
    if (u->futex_armed && !u->futex_wait_enosys) {
        s = krt_uring_get_sqe(u);
        if (s) {
            s->opcode    = IORING_OP_ASYNC_CANCEL;
            s->addr      = KRT_UD_FUTEX_WAIT;
            s->user_data = KRT_UD_FUTEX_CANCEL;
            (void)krt_uring_flush_sq(u);
        }
    }

    if (u->futex_wait_enosys)
        return PERL_KRT_ENOSYS;
    if (r < 0 && errno != ETIME && errno != EAGAIN)
        return KRT_NEG_ERRNO();
    return 0;
}

int
perl_krt_aio_notify(perl_krt_aio_t *ctx, uint64_t user_data)
{
    krt_uring_t *u;
    int rc;
    uint64_t one = 1;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    rc = perl_krt_mpsc_push(u->notify_ring, &user_data, sizeof user_data);
    if (rc)
        return rc;
    if (write(u->efd, &one, sizeof one) < 0 && errno != EAGAIN)
        return KRT_NEG_ERRNO();
    return 0;
}

int
perl_krt_aio_close(perl_krt_aio_t *ctx)
{
    krt_uring_t *u;
    if (!ctx || !ctx->impl)
        return -EINVAL;
    u = (krt_uring_t *)ctx->impl;
    if (u->sqes) munmap(u->sqes, *u->sq_entries * sizeof(struct io_uring_sqe));
    if (u->cq_ring && u->cq_ring != u->sq_ring) munmap(u->cq_ring, u->cq_ring_sz);
    if (u->sq_ring) munmap(u->sq_ring, u->sq_ring_sz);
    perl_krt_shm_destroy(&u->notify_shm);
    if (u->efd >= 0) close(u->efd);
    if (u->ring_fd >= 0) close(u->ring_fd);
    krt_reqtab_free(&u->reqs);
    free(u->statx_side);
    free(u->stash);
    free(u);
    ctx->impl = NULL;
    return 0;
}

#else
const int perl_krt_linux_backend = 0;   /* keeps the object non-empty */
#endif /* __linux__ */

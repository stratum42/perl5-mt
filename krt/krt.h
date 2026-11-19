/*    krt.h
 *
 *    perl5-mt kernel runtime adapter: the five-primitive OS contract that
 *    the multithreaded Perl core is written against.  Self-contained C11,
 *    no dependency on the Perl headers, no external libraries.
 *
 *    Primitives:
 *      1. shared memory segments that can be sealed read-only
 *      2. compartments (child processes in the same kernel, no shared VM)
 *      3. 32-bit wait/wake (futex / ulock)
 *      4. per-CPU identity (rseq where available)
 *      5. kernel-async I/O (io_uring / kqueue) with a cross-thread wakeup
 *
 *    Every function returns 0 on success and a negative errno on failure
 *    unless its comment says otherwise.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#ifndef PERL_KRT_H
#define PERL_KRT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- coherency granule -------------------------------------------------- */

#if defined(__powerpc64__) || defined(__ppc64__) || defined(_ARCH_PPC64)
#  define PERL_KRT_CACHE_LINE 128
#elif defined(__aarch64__) || defined(__arm64__)
#  define PERL_KRT_CACHE_LINE 128
#elif defined(__s390x__) || defined(__s390__) || defined(__zarch__) || defined(__MVS__)
#  define PERL_KRT_CACHE_LINE 256
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  define PERL_KRT_CACHE_LINE 64
#else
#  define PERL_KRT_CACHE_LINE 128
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(__IBMC__) || defined(__ibmxl__)
#  define PERL_KRT_CACHE_ALIGNED __attribute__((aligned(PERL_KRT_CACHE_LINE)))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__cplusplus)
#  define PERL_KRT_CACHE_ALIGNED _Alignas(PERL_KRT_CACHE_LINE)
#else
#  define PERL_KRT_CACHE_ALIGNED
#endif

/* ---- platform tag ------------------------------------------------------- */

#if defined(__linux__)
#  define PERL_KRT_LINUX 1
#elif defined(__APPLE__)
#  define PERL_KRT_DARWIN 1
#elif defined(_AIX)
#  define PERL_KRT_AIX 1
#  if defined(__PASE__) || defined(__OS400__)
#    define PERL_KRT_PASE 1
#  endif
#elif defined(__MVS__)
#  define PERL_KRT_ZOS 1
#  if (defined(__CHARSET_LIB) && (__CHARSET_LIB == 1)) || ('A' == 0x41)
#    define PERL_KRT_ZOS_ASCII 1
#  else
#    define PERL_KRT_ZOS_EBCDIC 1
#  endif
#else
#  define PERL_KRT_UNKNOWN 1
#endif

/* ---- endianness and character set --------------------------------------- */

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#  define PERL_KRT_BIG_ENDIAN 1
#elif defined(_AIX) || defined(__MVS__) || defined(__s390x__) || defined(__s390__) || \
      (defined(__powerpc64__) && !defined(__LITTLE_ENDIAN__))
#  define PERL_KRT_BIG_ENDIAN 1
#else
#  define PERL_KRT_BIG_ENDIAN 0
#endif

#if defined(EBCDIC) || defined(PERL_KRT_ZOS_EBCDIC) || ('A' == 0xC1)
#  define PERL_KRT_EBCDIC 1
#else
#  define PERL_KRT_EBCDIC 0
#endif

/* ---- errors ------------------------------------------------------------- */

#define PERL_KRT_ENOSYS   (-38)   /* primitive not implemented on this OS   */

/* ---- 1. shared memory --------------------------------------------------- */

typedef struct perl_krt_shm {
    int       fd;          /* memfd (Linux) / -1 (Darwin)                    */
    int       ro_fd;       /* read-only alias used after sealing (Linux)     */
    void     *base;        /* mapping in the creating process                */
    size_t    len;         /* bytes, page rounded                            */
    uint32_t  flags;       /* PERL_KRT_SHM_*                                 */
    uint64_t  os_handle;   /* Mach memory entry port (Darwin), else 0        */
} perl_krt_shm_t;

#define PERL_KRT_SHM_SEALED      0x1u   /* set by perl_krt_shm_seal_ro        */
#define PERL_KRT_SHM_NOINHERIT   0x2u   /* create: do not share with children */

/* Create a segment of `size` bytes, mapped read/write at out->base.        */
int  perl_krt_shm_create(perl_krt_shm_t *out, size_t size, uint32_t flags);

/* Make [addr, addr+len) read-only.  When the range covers the whole
 * segment the backing object itself is sealed: no process can ever map
 * it writable again, and a write through any existing mapping faults.
 * The mapping address does not change.                                    */
int  perl_krt_shm_seal_ro(perl_krt_shm_t *shm, void *addr, size_t len);

/* Map the segment a second time (or in a compartment) with `prot`
 * (PROT_READ, optionally | PROT_WRITE when not sealed).                    */
int  perl_krt_shm_remap(const perl_krt_shm_t *shm, void **addr_out, int prot);

/* Unmap base and release the backing object.                              */
int  perl_krt_shm_destroy(perl_krt_shm_t *shm);

/* Unmap a mapping obtained from perl_krt_shm_remap.                       */
int  perl_krt_shm_unmap(void *addr, size_t len);

/* Seal the size of the segment (no shrink, no grow) and open the read-only
 * alias that compartments map, without touching the protection of the
 * creating process's mapping.  Used together with protection keys, where
 * the mapping must stay PROT_WRITE for the key to govern it.           */
int  perl_krt_shm_seal_size(perl_krt_shm_t *shm);

/* ---- 1b. memory protection keys ----------------------------------------- */

/* A protection key (Linux pkeys: PKU on x86, AMR on POWER).  Every thread
 * decides for itself, with perl_krt_key_arm, whether memory bound to the
 * key may be written (or accessed at all); new threads inherit the state
 * of their creator, and a signal handler on x86 runs with every allocated
 * key access-disabled, so code that longjmps out of a handler re-arms.
 * -1 is "no key".  Darwin and the stubs return PERL_KRT_ENOSYS.          */
typedef int perl_krt_key_t;

#define PERL_KRT_KEY_NONE       (-1)
#define PERL_KRT_KEY_NOWRITE    0x1u
#define PERL_KRT_KEY_NOACCESS   0x2u

int  perl_krt_key_alloc(perl_krt_key_t *out);
int  perl_krt_key_free(perl_krt_key_t key);
/* flags: 0 (read/write), PERL_KRT_KEY_NOWRITE or PERL_KRT_KEY_NOACCESS;
 * affects the calling thread only.                                       */
int  perl_krt_key_arm(perl_krt_key_t key, uint32_t flags);
/* Bind the whole mapping of shm to the key (pkey_mprotect, read/write). */
int  perl_krt_shm_bind_key(perl_krt_shm_t *shm, perl_krt_key_t key);

/* ---- 2. compartments ---------------------------------------------------- */

typedef int (*perl_krt_comp_fn)(void *arg);

typedef struct perl_krt_comp {
    pid_t  pid;
    int    ctl_fd;      /* parent's end of a SOCK_STREAM socketpair          */
    int    exited;      /* set by perl_krt_comp_reap                         */
    int    status;      /* raw wait status                                   */
} perl_krt_comp_t;

/* Spawn a child that runs fn(arg) and exits with its return value.  The
 * child shares the kernel, the shm segments listed in `segs` and the
 * parent's descriptors, but never the parent's virtual memory.  Inside
 * the child, perl_krt_comp_ctl_fd() returns its end of the control
 * socket.
 *
 * flags: PERL_KRT_COMP_SHARE_FILES asks for a *shared* descriptor table
 * (Linux CLONE_FILES); the default copies the table at spawn time, which
 * keeps a compartment's close() from closing the host's descriptors.      */
int  perl_krt_comp_spawn(perl_krt_comp_t *out, perl_krt_comp_fn fn, void *arg,
                         const perl_krt_shm_t *segs, size_t n_segs);

#define PERL_KRT_COMP_SHARE_FILES   0x1u
int  perl_krt_comp_spawn_flags(perl_krt_comp_t *out, perl_krt_comp_fn fn,
                               void *arg, const perl_krt_shm_t *segs,
                               size_t n_segs, uint32_t flags);

/* In a compartment: the child's end of the control socket; -1 in the
 * parent.                                                                 */
int  perl_krt_comp_ctl_fd(void);

/* Wait for the compartment.  With PERL_KRT_REAP_NOHANG returns -EAGAIN
 * while it is still running.  *status receives the exit code (or
 * 128+signal).                                                            */
#define PERL_KRT_REAP_NOHANG  0x1u
int  perl_krt_comp_reap(perl_krt_comp_t *comp, int *status);
int  perl_krt_comp_reap_flags(perl_krt_comp_t *comp, int *status,
                              uint32_t flags);

/* Ask the compartment to terminate (SIGTERM, then SIGKILL after a grace
 * period handled by the caller).                                          */
int  perl_krt_comp_kill(perl_krt_comp_t *comp, int sig);

/* ---- 3. wait / wake ----------------------------------------------------- */

#define PERL_KRT_WAIT_FOREVER  UINT64_MAX

/* Block while *seq == expected, for at most timeout_ns.  Spurious returns
 * are allowed; callers loop.                                              */
void perl_krt_wait_u32(volatile uint32_t *seq, uint32_t expected,
                       uint64_t timeout_ns);

/* Wake up to `count` waiters on seq (UINT32_MAX = all).                   */
void perl_krt_wake_u32(volatile uint32_t *seq, uint32_t count);

/* The same for a word that other processes (compartments) wait on or wake
 * through a shared segment: a process-private futex or ulock is keyed by
 * the address space and never crosses a fork, so such words must use
 * PERL_KRT_WAIT_SHARED on both sides.                                     */
#define PERL_KRT_WAIT_SHARED  0x1u
void perl_krt_wait_u32_flags(volatile uint32_t *seq, uint32_t expected,
                             uint64_t timeout_ns, uint32_t flags);
void perl_krt_wake_u32_flags(volatile uint32_t *seq, uint32_t count,
                             uint32_t flags);

/* ---- 4. per-CPU identity ------------------------------------------------ */

/* Current CPU number, or -1 when unknown.  Uses rseq on Linux (no
 * syscall), sched_getcpu / sysctl otherwise.                              */
int  perl_krt_cpu_id(void);

/* Number of CPUs the process may run on (>= 1).                           */
int  perl_krt_cpu_count(void);

/* Pin the calling thread to one CPU (Linux sched_setaffinity; the kernel
 * does not pin on Darwin: PERL_KRT_ENOSYS).                               */
int  perl_krt_cpu_pin(int cpu);

/* The other hardware threads of cpu's core, written to out[0..n).  Returns
 * n (cpu itself is not listed), 0 when cpu has no SMT sibling, or -errno
 * (PERL_KRT_ENOSYS on Darwin).  Reads thread_siblings_list in sysfs.     */
int  perl_krt_smt_siblings(int cpu, int *out, int max);

/* The NUMA node of cpu, or -1 when unknown (Darwin: always -1).          */
int  perl_krt_numa_node(int cpu);

/* ---- 5. async I/O ------------------------------------------------------- */

enum perl_krt_aio_op {
    PERL_KRT_AIO_NOP = 0,
    PERL_KRT_AIO_OPENAT,     /* fd = dirfd, addr = const char *path,
                                open_flags, mode                            */
    PERL_KRT_AIO_STATX,      /* fd = dirfd, addr = const char *path ("" +
                                AT_EMPTY_PATH in open_flags stats fd),
                                addr2 = perl_krt_stat_t *                   */
    PERL_KRT_AIO_READ,       /* fd, addr, len, off (-1 = current)           */
    PERL_KRT_AIO_WRITE,
    PERL_KRT_AIO_FSYNC,
    PERL_KRT_AIO_SPLICE,     /* fd -> fd2, len bytes, off / off2            */
    PERL_KRT_AIO_SEND_ZC,    /* socket send, zero copy where supported      */
    PERL_KRT_AIO_CLOSE,
    PERL_KRT_AIO_OP_MAX
};

/* Portable stat result written by PERL_KRT_AIO_STATX (sqe.addr2).        */
typedef struct perl_krt_stat {
    uint64_t dev, ino, nlink, mode, uid, gid, rdev, size, blksize, blocks;
    int64_t  atime_ns, mtime_ns, ctime_ns, btime_ns;   /* since the epoch */
} perl_krt_stat_t;

#define PERL_KRT_SQE_FIXED_FILE  0x1u   /* fd is an index into registered files */
#define PERL_KRT_SQE_FIXED_BUF   0x2u   /* addr is an offset in reg_buf          */

typedef struct perl_krt_sqe {
    uint8_t   op;
    uint8_t   flags;
    uint16_t  buf_index;
    int32_t   fd;
    uint64_t  off;
    uint64_t  addr;
    uint32_t  len;
    uint32_t  open_flags;
    int32_t   fd2;
    uint32_t  mode;
    uint64_t  off2;
    uint64_t  addr2;
    uint64_t  user_data;
} perl_krt_sqe_t;

#define PERL_KRT_CQE_NOTIFY  0x1u   /* produced by perl_krt_aio_notify     */

typedef struct perl_krt_cqe {
    uint64_t  user_data;
    int32_t   res;          /* bytes / fd / 0, or -errno                   */
    uint32_t  flags;
} perl_krt_cqe_t;

typedef struct perl_krt_aio {
    uint32_t         depth;
    uint32_t         flags;
    perl_krt_shm_t  *reg_buf;   /* optional registered buffer segment      */
    void            *impl;      /* OS private                              */
} perl_krt_aio_t;

#define PERL_KRT_AIO_SQPOLL   0x1u   /* Linux: IORING_SETUP_SQPOLL         */

int  perl_krt_aio_init(perl_krt_aio_t *ctx, uint32_t depth,
                       perl_krt_shm_t *reg_buf);
int  perl_krt_aio_init_flags(perl_krt_aio_t *ctx, uint32_t depth,
                             perl_krt_shm_t *reg_buf, uint32_t flags);
int  perl_krt_aio_register_files(perl_krt_aio_t *ctx, const int *fds,
                                 uint32_t n);
/* Returns the number of sqes accepted (0..n) or -errno.                   */
int  perl_krt_aio_submit(perl_krt_aio_t *ctx, const perl_krt_sqe_t *sqes,
                         uint32_t n);
/* Returns the number of cqes written (0..max) or -errno; waits up to
 * timeout_ns for at least one when none is ready.                         */
int  perl_krt_aio_drain(perl_krt_aio_t *ctx, perl_krt_cqe_t *cqes,
                        uint32_t max, uint64_t timeout_ns);
/* nonzero when the ring can take SQE_FIXED_BUF operations.  Zero
 * when the registered buffers were refused (locked memory): use private
 * buffers then.                                                           */
int  perl_krt_aio_fixed_bufs(const perl_krt_aio_t *ctx);
/* nonzero when a completion is waiting for perl_krt_aio_drain.
 * Reads the ring's indices only (no system call on Linux); the poll of an
 * idle loop uses it.                                                      */
int  perl_krt_aio_ready(const perl_krt_aio_t *ctx);
/* block until a completion is ready or timeout_ns passes (0 =
 * return at once, PERL_KRT_WAIT_FOREVER = no timeout).  Takes nothing: the
 * next drain does.  Returns 0 or -errno.                                  */
int  perl_krt_aio_wait(perl_krt_aio_t *ctx, uint64_t timeout_ns);
/* Post a PERL_KRT_CQE_NOTIFY completion carrying user_data into ctx from
 * any thread or compartment that shares the process.  Wakes a drain.      */
int  perl_krt_aio_notify(perl_krt_aio_t *ctx, uint64_t user_data);
/* A descriptor that becomes readable when completions are pending; for
 * integrating with foreign event loops.  -1 when unsupported.             */
int  perl_krt_aio_wakeup_fd(const perl_krt_aio_t *ctx);
int  perl_krt_aio_close(perl_krt_aio_t *ctx);

/* ---- misc --------------------------------------------------------------- */

/* Monotonic nanoseconds. */
uint64_t perl_krt_now_ns(void);

/* System page size. */
size_t perl_krt_page_size(void);

/* Short human readable platform description ("linux/io_uring", ...).      */
const char *perl_krt_platform(void);

#ifdef __cplusplus
}
#endif

#endif /* PERL_KRT_H */

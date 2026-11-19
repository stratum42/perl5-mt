/*    perlio_async.c
 *    perl5-mt: the :async PerlIO layer over the krt aio ring, and
 *    the XSUBs MT::IO::stat, splice, fsync and aio_stats.
 *
 *    A handle opened with :async does its own descriptor I/O: open, read,
 *    write, fsync, stat, close go through perl_krt_aio_submit (io_uring on
 *    Linux, the libdispatch pool on macOS) and do not block the thread when
 *    the caller is a scheduled fiber.  The layer is buffered (PerlIOBuf
 *    fields) and the buffer is one of PERL_MT_AIO_BUFS registered 64 KiB
 *    buffers of the interpreter's ring when one is free, so the kernel does
 *    not copy from it to the page cache; otherwise a private buffer and
 *    plain READ/WRITE.
 *
 *    Position.  Every descriptor operation names its offset (or -1 for the
 *    current position of a stream such as a pipe), so the kernel's file
 *    position is never consulted.  `off` is the file offset of the buffer's
 *    first byte; the logical position of the handle is off + (ptr - buf).
 *
 *    Completion.  Each operation has a user_data key (PL_mt_aio_next_key).
 *    A scheduled fiber records its key on its record, links itself on
 *    PL_mt_aio_waiters and parks; the loop's idle or poll hook drains the
 *    ring (Perl_mt_aio_drain_completions) and wakes it with the completion
 *    in aio_cqe.  A thread that is not a parkable fiber drains the ring
 *    itself until its own key completes, and hands any other completion to
 *    its waiter on the way.  The loop and the drains are in mt_fiber.c.
 *
 *    Limits.  Only connected sockets are read and written: accept and
 *    connect are not in the operation set.  Dup is not supported.  The
 *    layer's buffer is not shared between handles.
 *
 *    This file is part of perl5-mt and is distributed under the same terms
 *    as Perl itself (Artistic License or GNU GPL).
 */

#include "EXTERN.h"
#define PERL_IN_PERLIO_ASYNC_C
#include "perl.h"
#include "XSUB.h"
#include "perliol.h"
#include "mt_fiber.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Linux stats the empty path of a descriptor with AT_EMPTY_PATH; macOS has no
 * such flag and krt fstat()s the descriptor when the path is empty. */
#ifndef AT_EMPTY_PATH
#  define AT_EMPTY_PATH 0
#endif

/* The registered buffers of an interpreter's ring, and the depth of its
 * submission queue (PERL_MT_AIO_BUFS: one bit each in PL_mt_aio_bufs_free). */
#define PERL_MT_AIO_BUFS      32
#define PERL_MT_AIO_BUF_SIZE  (64 * 1024)
#define PERL_MT_AIO_DEPTH     256
#define PERL_MT_AIO_DRAIN     64

/* The per-handle state: the buffered layer's struct, then ours. */
typedef struct {
    PerlIOBuf base;
    int       fd;           /* -1 when closed                          */
    int       buf_idx;      /* registered buffer, or -1 (private)      */
    Off_t     off;          /* file offset of buf[0]                    */
    int       seekable;     /* regular file: offsets are meaningful     */
} PerlIOAsync;

#define PERLIO_ASYNC_SELF(f) PerlIOSelf((f), PerlIOAsync)

/* ---- the ring -------------------------------------------------------------- */

/* Creates the interpreter's ring on first use.  Croaks when the kernel has
 * no aio (the krt stubs answer ENOSYS). */
static void
S_aio_init(pTHX)
{
    if (PL_mt_aio)
        return;
    Newxz(PL_mt_aio_shm, 1, perl_krt_shm_t);
    if (perl_krt_shm_create(PL_mt_aio_shm,
                            (size_t)PERL_MT_AIO_BUFS * PERL_MT_AIO_BUF_SIZE,
                            0) != 0) {
        Safefree(PL_mt_aio_shm);
        PL_mt_aio_shm = NULL;
        Perl_croak(aTHX_ "MT :async: no shared buffers for the aio ring");
    }
    Newxz(PL_mt_aio, 1, perl_krt_aio_t);
    if (perl_krt_aio_init(PL_mt_aio, PERL_MT_AIO_DEPTH, PL_mt_aio_shm) != 0) {
        Safefree(PL_mt_aio);
        PL_mt_aio = NULL;
        perl_krt_shm_destroy(PL_mt_aio_shm);
        Safefree(PL_mt_aio_shm);
        PL_mt_aio_shm = NULL;
        Perl_croak(aTHX_ "MT :async: no aio on this kernel (%s)",
                   perl_krt_platform());
    }
    PL_mt_aio_bufs_free = (PERL_MT_AIO_BUFS == 32)
        ? 0xffffffffu : ((1u << PERL_MT_AIO_BUFS) - 1u);
}

/* A new interpreter that was cloned from another one: forget the source's
 * ring and waiters without touching them (they belong to the source).  The
 * clone makes its own ring on first use. */
void
Perl_mt_aio_reset(pTHX)
{
    PL_mt_aio = NULL;
    PL_mt_aio_shm = NULL;
    PL_mt_aio_bufs_free = 0;
    PL_mt_aio_next_key = 1;
    PL_mt_aio_waiters = NULL;
    PL_mt_aio_pending = 0;
    PL_mt_aio_sync_key = 0;
    PL_mt_aio_nsubmit = 0;
    PL_mt_aio_ndone = 0;
    PL_mt_aio_nparked = 0;
    PL_mt_aio_nsync = 0;
}

/* Frees the ring of an interpreter that is going away.  Operations still in
 * flight are abandoned: their completions are never delivered. */
void
Perl_mt_aio_destroy(pTHX)
{
    if (!PL_mt_aio)
        return;
    (void)perl_krt_aio_close(PL_mt_aio);
    Safefree(PL_mt_aio);
    PL_mt_aio = NULL;
    if (PL_mt_aio_shm) {
        (void)perl_krt_shm_destroy(PL_mt_aio_shm);
        Safefree(PL_mt_aio_shm);
        PL_mt_aio_shm = NULL;
    }
    PL_mt_aio_waiters = NULL;
    PL_mt_aio_pending = 0;
    PL_mt_aio_sync_key = 0;
}

/* Hands a completion to the thread or fiber that waits for it.  A completion
 * nobody waits for (its layer was closed under it) is dropped. */
static void
S_aio_deliver(pTHX_ const perl_krt_cqe_t *c, int *woke)
{
    struct mt_fiber *w, *prev = NULL;
    if (PL_mt_aio_pending)
        PL_mt_aio_pending--;
    PL_mt_aio_ndone++;
    if (PL_mt_aio_sync_key && c->user_data == PL_mt_aio_sync_key) {
        PL_mt_aio_sync_cqe = *c;
        PL_mt_aio_sync_key = 0;
        return;
    }
    for (w = PL_mt_aio_waiters; w; prev = w, w = w->next_aio_waiter)
        if (w->aio_key == c->user_data)
            break;
    if (!w)
        return;
    if (prev)
        prev->next_aio_waiter = w->next_aio_waiter;
    else
        PL_mt_aio_waiters = w->next_aio_waiter;
    w->next_aio_waiter = NULL;
    w->aio_cqe = *c;
    w->aio_flags |= MT_FIBER_AIO_DONE;
    Perl_mt_sched_wake(aTHX_ w);
    if (woke)
        (*woke)++;
}

/* The non-blocking drain: delivers every completion that is ready.  Called by
 * the scheduler's poll and idle hooks (mt_fiber.c) and by the synchronous
 * waits below.  *woke, when not NULL, gets the number of fibers it woke. */
void
Perl_mt_aio_drain_completions(pTHX_ int *woke)
{
    perl_krt_cqe_t cqes[PERL_MT_AIO_DRAIN];
    int n, i;
    if (!PL_mt_aio)
        return;
    n = perl_krt_aio_drain(PL_mt_aio, cqes, PERL_MT_AIO_DRAIN, 0);
    for (i = 0; i < n; i++)
        S_aio_deliver(aTHX_ &cqes[i], woke);
}

/* Submits one operation and waits for its completion.  Returns the result
 * (bytes, a fd, 0) or -errno.  In a parkable fiber the fiber parks until the
 * loop delivers the completion; otherwise this thread drains the ring until
 * the key comes back.  Public for the other mt files (MT::IO and the
 * buffer layer use it through the same ring). */
IV
Perl_mt_aio_run(pTHX_ perl_krt_sqe_t *q)
{
    PERL_ARGS_ASSERT_MT_AIO_RUN;
    struct mt_fiber * const f = PL_mt_fiber_current;
    int n;
    S_aio_init(aTHX);
    q->user_data = PL_mt_aio_next_key++;
    for (;;) {
        n = perl_krt_aio_submit(PL_mt_aio, q, 1);
        if (n == 1)
            break;
        if (n < 0)
            return n;
        /* the submission queue is full: take the completions that are
         * ready, or wait for one */
        (void)perl_krt_aio_wait(PL_mt_aio, 1000000ull);
        Perl_mt_aio_drain_completions(aTHX_ NULL);
    }
    PL_mt_aio_pending++;
    PL_mt_aio_nsubmit++;

    if (MT_FIBER_PARKABLE(f, PL_mt_sched_fiber)) {
        perl_krt_cqe_t cqe;
        f->aio_key = q->user_data;
        f->aio_flags = 0;
        f->next_aio_waiter = PL_mt_aio_waiters;
        PL_mt_aio_waiters = f;
        PL_mt_aio_nparked++;
        Perl_mt_sched_arm(aTHX);
        do {
            Perl_mt_sched_park(aTHX);
        } while (!(f->aio_flags & MT_FIBER_AIO_DONE));
        cqe = f->aio_cqe;
        f->aio_flags = 0;
        return cqe.res;
    }

    PL_mt_aio_sync_key = q->user_data;
    while (PL_mt_aio_sync_key) {
        if (!perl_krt_aio_ready(PL_mt_aio))
            (void)perl_krt_aio_wait(PL_mt_aio, 1000000000ull);
        Perl_mt_aio_drain_completions(aTHX_ NULL);
    }
    PL_mt_aio_nsync++;
    return PL_mt_aio_sync_cqe.res;
}

/* ---- buffers --------------------------------------------------------------- */

/* A free registered buffer's index, or -1 when all are in use. */
static int
S_buf_take(pTHX)
{
    U32 i;
    for (i = 0; i < PERL_MT_AIO_BUFS; i++) {
        if (PL_mt_aio_bufs_free & (1u << i)) {
            PL_mt_aio_bufs_free &= ~(1u << i);
            return (int)i;
        }
    }
    return -1;
}

static void
S_buf_give(pTHX_ int idx)
{
    if (idx >= 0)
        PL_mt_aio_bufs_free |= (1u << idx);
}

/* How many of the registered buffers are free. */
static U32
S_bufs_free(pTHX)
{
    U32 i, n = 0;
    for (i = 0; i < PERL_MT_AIO_BUFS; i++)
        if (PL_mt_aio_bufs_free & (1u << i))
            n++;
    return n;
}

/* Sets the operation's buffer: a registered one is named by its offset in
 * the ring's buffer segment, a private one by its address. */
static void
S_sqe_buf(pTHX_ perl_krt_sqe_t *q, const PerlIOAsync *a, const STDCHAR *p)
{
    if (a->buf_idx >= 0) {
        /* the offset of p in the ring's buffer segment */
        q->flags |= PERL_KRT_SQE_FIXED_BUF;
        q->addr = (uint64_t)(p - (const STDCHAR *)PL_mt_aio_shm->base);
    }
    else {
        q->addr = PTR2UV(p);
    }
}

/* ---- the layer --------------------------------------------------------------- */

static IV PerlIOAsync_seek_(pTHX_ PerlIO *f, Off_t offset, int whence);
static STDCHAR *PerlIOAsync_get_base_(pTHX_ PerlIO *f);

/* Records the descriptor on the layer and asks whether it is a regular file
 * (only those have meaningful offsets). */
static void
S_setfd(pTHX_ PerlIO *f, int fd)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    struct stat st;
    a->fd = fd;
    a->seekable = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
    a->off = a->seekable ? lseek(fd, 0, SEEK_CUR) : 0;
}

static IV
PerlIOAsync_pushed(pTHX_ PerlIO *f, const char *mode, SV *arg, PerlIO_funcs *tab)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    a->fd = -1;
    a->buf_idx = -1;
    a->off = 0;
    a->seekable = 0;
    /* pushed onto an open handle (binmode): the descriptor is the layer
     * below's; its buffer is flushed and its read-ahead is dropped */
    if (PerlIOValid(PerlIONext(f))) {
        const int fd = PerlIO_fileno(PerlIONext(f));
        if (fd >= 0) {
            (void)PerlIO_flush(PerlIONext(f));
            S_setfd(aTHX_ f, fd);
        }
    }
    return PerlIOBase_pushed(aTHX_ f, mode, arg, tab);
}

static IV
PerlIOAsync_popped(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    if (a->buf_idx >= 0) {
        S_buf_give(aTHX_ a->buf_idx);
        a->buf_idx = -1;
    }
    else if (b->buf) {
        Safefree(b->buf);
    }
    b->buf = NULL;
    return PerlIOBase_popped(aTHX_ f);
}

/* The mode letters open(2) takes, as PerlIOUnix_oflags reads them (the
 * "r", "w", "a" letters perl passes, with "+" for read-write). */
static int
S_oflags(const char *mode)
{
    if (*mode == IoTYPE_IMPLICIT || *mode == IoTYPE_NUMERIC)
        mode++;
    switch (*mode) {
    case 'r':
        return (mode[1] == '+') ? O_RDWR : O_RDONLY;
    case 'w':
        return (mode[1] == '+') ? (O_RDWR | O_CREAT | O_TRUNC)
                                : (O_WRONLY | O_CREAT | O_TRUNC);
    case 'a':
        return (mode[1] == '+') ? (O_RDWR | O_CREAT) : (O_WRONLY | O_CREAT);
    default:
        return -1;
    }
}

static PerlIO *
PerlIOAsync_open(pTHX_ PerlIO_funcs *self, PerlIO_list_t *layers, IV n,
                 const char *mode, int fd, int imode, int perm,
                 PerlIO *f, int narg, SV **args)
{
    if (PerlIOValid(f)) {
        if (PerlIOBase(f)->tab && (PerlIOBase(f)->flags & PERLIO_F_OPEN))
            (*PerlIOBase(f)->tab->Close)(aTHX_ f);
    }
    if (narg > 0) {
        if (*mode == IoTYPE_NUMERIC)
            mode++;
        else {
            imode = S_oflags(mode);
            perm = 0666;
        }
        if (imode != -1) {
            STRLEN len;
            const char * const path = SvPV_const(*args, len);
            perl_krt_sqe_t q;
            IV res;
            if (!IS_SAFE_PATHNAME(path, len, "open"))
                return NULL;
            S_aio_init(aTHX);
            Zero(&q, 1, perl_krt_sqe_t);
            q.op = PERL_KRT_AIO_OPENAT;
            q.fd = AT_FDCWD;
            q.addr = PTR2UV(path);
            q.open_flags = (uint32_t)imode;
            q.mode = (uint32_t)perm;
            res = Perl_mt_aio_run(aTHX_ &q);
            if (res < 0) {
                SETERRNO((int)-res, LIB_INVARG);
                return NULL;
            }
            fd = (int)res;
        }
    }
    if (fd < 0)
        return NULL;
    if (*mode == IoTYPE_IMPLICIT)
        mode++;
    if (!f)
        f = PerlIO_allocate(aTHX);
    if (!PerlIOValid(f)) {
        if (!(f = PerlIO_push(aTHX_ f, self, mode, PerlIOArg))) {
            (void)PerlLIO_close(fd);
            return NULL;
        }
    }
    S_setfd(aTHX_ f, fd);
    PerlIOBase(f)->flags |= PERLIO_F_OPEN;
    if (*mode == IoTYPE_APPEND)
        (void)PerlIOAsync_seek_(aTHX_ f, 0, SEEK_END);
    return f;
}

/* The size of the file a descriptor names (STATX of the empty path), or -1
 * with errno set. */
static Off_t
S_fd_size(pTHX_ int fd)
{
    perl_krt_sqe_t q;
    perl_krt_stat_t st;
    IV res;
    Zero(&q, 1, perl_krt_sqe_t);
    Zero(&st, 1, perl_krt_stat_t);
    q.op = PERL_KRT_AIO_STATX;
    q.fd = fd;
    q.addr = PTR2UV("");
    q.open_flags = AT_EMPTY_PATH;
    q.addr2 = PTR2UV(&st);
    res = Perl_mt_aio_run(aTHX_ &q);
    if (res < 0) {
        SETERRNO((int)-res, LIB_INVARG);
        return -1;
    }
    return (Off_t)st.size;
}

/* Writes the buffer's pending bytes (from buf to ptr) at off, short writes
 * included.  Returns 0 or -1 with the error set on the handle. */
static int
S_write_out(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    STDCHAR *p = b->buf;
    while (p < b->ptr) {
        perl_krt_sqe_t q;
        IV res;
        Zero(&q, 1, perl_krt_sqe_t);
        q.op = PERL_KRT_AIO_WRITE;
        q.fd = a->fd;
        q.len = (uint32_t)(b->ptr - p);
        q.off = a->seekable ? (uint64_t)(a->off + (p - b->buf)) : (uint64_t)-1;
        S_sqe_buf(aTHX_ &q, a, p);
        res = Perl_mt_aio_run(aTHX_ &q);
        if (res <= 0) {
            PerlIOBase(f)->flags |= PERLIO_F_ERROR;
            SETERRNO(res < 0 ? (int)-res : EIO, LIB_INVARG);
            return -1;
        }
        p += res;
    }
    if (a->seekable)
        a->off += (Off_t)(b->ptr - b->buf);
    b->ptr = b->end = b->buf;
    PerlIOBase(f)->flags &= ~PERLIO_F_WRBUF;
    return 0;
}

static IV
PerlIOAsync_flush(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    if (!b->buf)
        return 0;
    if (PerlIOBase(f)->flags & PERLIO_F_WRBUF)
        return S_write_out(aTHX_ f);
    if (PerlIOBase(f)->flags & PERLIO_F_RDBUF) {
        /* drop the read-ahead: the logical position is the new offset */
        if (a->seekable)
            a->off += (Off_t)(b->ptr - b->buf);
        b->ptr = b->end = b->buf;
        PerlIOBase(f)->flags &= ~PERLIO_F_RDBUF;
    }
    return 0;
}

/* Reads one buffer at the current offset.  Returns 0, or -1 at end of file
 * (PERLIO_F_EOF) or on an error. */
static IV
PerlIOAsync_fill(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    perl_krt_sqe_t q;
    IV res;
    if (PerlIOAsync_flush(aTHX_ f) != 0)
        return -1;
    if (!b->buf)
        (void)PerlIOAsync_get_base_(aTHX_ f);
    if (a->fd < 0) {
        SETERRNO(EBADF, LIB_INVARG);
        return -1;
    }
    b->ptr = b->end = b->buf;
    Zero(&q, 1, perl_krt_sqe_t);
    q.op = PERL_KRT_AIO_READ;
    q.fd = a->fd;
    q.len = (uint32_t)b->bufsiz;
    q.off = a->seekable ? (uint64_t)a->off : (uint64_t)-1;
    S_sqe_buf(aTHX_ &q, a, b->buf);
    res = Perl_mt_aio_run(aTHX_ &q);
    if (res < 0) {
        PerlIOBase(f)->flags |= PERLIO_F_ERROR;
        SETERRNO((int)-res, LIB_INVARG);
        return -1;
    }
    if (res == 0) {
        PerlIOBase(f)->flags |= PERLIO_F_EOF;
        return -1;
    }
    b->end = b->buf + res;
    PerlIOBase(f)->flags |= PERLIO_F_RDBUF;
    return 0;
}

static SSize_t
PerlIOAsync_read(pTHX_ PerlIO *f, void *vbuf, Size_t count)
{
    if (PerlIOValid(f)) {
        const PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
        if (!b->buf)
            (void)PerlIOAsync_get_base_(aTHX_ f);
        return PerlIOBase_read(aTHX_ f, vbuf, count);
    }
    return 0;
}

/* Copies into the buffer and writes it out when it is full.  Reads ahead
 * are dropped first: the position is the logical one. */
static SSize_t
PerlIOAsync_write(pTHX_ PerlIO *f, const void *vbuf, Size_t count)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    const STDCHAR *p = (const STDCHAR *)vbuf;
    SSize_t done = 0;
    if (a->fd < 0) {
        SETERRNO(EBADF, LIB_INVARG);
        return -1;
    }
    if (!b->buf)
        (void)PerlIOAsync_get_base_(aTHX_ f);
    if (PerlIOBase(f)->flags & PERLIO_F_RDBUF) {
        if (a->seekable)
            a->off += (Off_t)(b->ptr - b->buf);
        b->ptr = b->end = b->buf;
        PerlIOBase(f)->flags &= ~PERLIO_F_RDBUF;
    }
    while (count > 0) {
        size_t room = b->bufsiz - (size_t)(b->ptr - b->buf);
        size_t n;
        if (room == 0) {
            if (S_write_out(aTHX_ f) != 0)
                return done ? done : -1;
            room = b->bufsiz;
        }
        n = count < room ? count : room;
        Copy(p, b->ptr, n, STDCHAR);
        b->ptr += n;
        p += n;
        count -= n;
        done += (SSize_t)n;
    }
    PerlIOBase(f)->flags |= PERLIO_F_WRBUF;
    return done;
}

static IV
PerlIOAsync_seek_(pTHX_ PerlIO *f, Off_t offset, int whence)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    Off_t target;
    if (!a->seekable) {
        SETERRNO(ESPIPE, LIB_INVARG);
        return -1;
    }
    if (PerlIOAsync_flush(aTHX_ f) != 0)
        return -1;
    switch (whence) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = a->off + offset;
        break;
    case SEEK_END: {
        const Off_t size = S_fd_size(aTHX_ a->fd);
        if (size < 0)
            return -1;
        target = size + offset;
        break;
    }
    default:
        SETERRNO(EINVAL, LIB_INVARG);
        return -1;
    }
    if (target < 0) {
        SETERRNO(EINVAL, LIB_INVARG);
        return -1;
    }
    a->off = target;
    b->ptr = b->end = b->buf;
    PerlIOBase(f)->flags &= ~(PERLIO_F_EOF | PERLIO_F_RDBUF | PERLIO_F_WRBUF);
    return 0;
}

static IV
PerlIOAsync_seek(pTHX_ PerlIO *f, Off_t offset, int whence)
{
    return PerlIOAsync_seek_(aTHX_ f, offset, whence);
}

static Off_t
PerlIOAsync_tell(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    if (!a->seekable) {
        SETERRNO(ESPIPE, LIB_INVARG);
        return -1;
    }
    return a->off + (b->buf ? (Off_t)(b->ptr - b->buf) : 0);
}

static IV
PerlIOAsync_close(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    IV code = 0;
    if (PerlIOAsync_flush(aTHX_ f) != 0)
        code = -1;
    if (a->fd >= 0) {
        perl_krt_sqe_t q;
        IV res;
        Zero(&q, 1, perl_krt_sqe_t);
        q.op = PERL_KRT_AIO_CLOSE;
        q.fd = a->fd;
        res = Perl_mt_aio_run(aTHX_ &q);
        if (res < 0) {
            SETERRNO((int)-res, LIB_INVARG);
            code = -1;
        }
        a->fd = -1;
    }
    if (a->buf_idx >= 0) {
        S_buf_give(aTHX_ a->buf_idx);
        a->buf_idx = -1;
        b->buf = NULL;
    }
    else if (b->buf) {
        Safefree(b->buf);
        b->buf = NULL;
    }
    b->ptr = b->end = NULL;
    PerlIOBase(f)->flags &= ~(PERLIO_F_OPEN | PERLIO_F_RDBUF | PERLIO_F_WRBUF);
    return code;
}

/* The buffer: a registered one of the ring when one is free, else private. */
static STDCHAR *
PerlIOAsync_get_base_(pTHX_ PerlIO *f)
{
    PerlIOAsync * const a = PERLIO_ASYNC_SELF(f);
    PerlIOBuf * const b = PerlIOSelf(f, PerlIOBuf);
    if (!b->buf) {
        S_aio_init(aTHX);
        if (a->buf_idx < 0 && perl_krt_aio_fixed_bufs(PL_mt_aio))
            a->buf_idx = S_buf_take(aTHX);
        if (a->buf_idx >= 0)
            b->buf = (STDCHAR *)PL_mt_aio_shm->base
                     + (size_t)a->buf_idx * PERL_MT_AIO_BUF_SIZE;
        else
            Newx(b->buf, PERL_MT_AIO_BUF_SIZE, STDCHAR);
        b->bufsiz = PERL_MT_AIO_BUF_SIZE;
        b->ptr = b->end = b->buf;
    }
    return b->buf;
}

static STDCHAR *
PerlIOAsync_get_base(pTHX_ PerlIO *f)
{
    return PerlIOAsync_get_base_(aTHX_ f);
}

static IV
PerlIOAsync_fileno(pTHX_ PerlIO *f)
{
    return PERLIO_ASYNC_SELF(f)->fd;
}

/* Not supported: a second descriptor would need its own ring buffer and a
 * position of its own, and no caller needs one yet. */
static PerlIO *
PerlIOAsync_dup(pTHX_ PerlIO *f, PerlIO *o, CLONE_PARAMS *param, int flags)
{
    PERL_UNUSED_ARG(f);
    PERL_UNUSED_ARG(o);
    PERL_UNUSED_ARG(param);
    PERL_UNUSED_ARG(flags);
    SETERRNO(ENOSYS, LIB_INVARG);
    return NULL;
}

PERLIO_FUNCS_DECL(PerlIO_async) = {
    sizeof(PerlIO_funcs),
    "async",
    sizeof(PerlIOAsync),
    PERLIO_K_BUFFERED|PERLIO_K_RAW,
    PerlIOAsync_pushed,
    PerlIOAsync_popped,
    PerlIOAsync_open,
    PerlIOBase_binmode,
    NULL,                       /* getarg */
    PerlIOAsync_fileno,
    PerlIOAsync_dup,
    PerlIOAsync_read,
    NULL,                       /* unread */
    PerlIOAsync_write,
    PerlIOAsync_seek,
    PerlIOAsync_tell,
    PerlIOAsync_close,
    PerlIOAsync_flush,
    PerlIOAsync_fill,
    PerlIOBase_eof,
    PerlIOBase_error,
    PerlIOBase_clearerr,
    PerlIOBase_setlinebuf,
    PerlIOAsync_get_base,
    PerlIOBuf_bufsiz,
    PerlIOBuf_get_ptr,
    PerlIOBuf_get_cnt,
    PerlIOBuf_set_ptrcnt,
};

/* ---- the XSUBs -------------------------------------------------------------- */

/* MT::IO::stat($path): the thirteen values stat returns, or the empty list
 * with $! set. */
XS(XS_MT_IO_stat)
{
    dXSARGS;
    STRLEN len;
    const char *path;
    perl_krt_sqe_t q;
    perl_krt_stat_t st;
    IV res;
    if (items != 1)
        croak_xs_usage(cv, "path");
    path = SvPV_const(ST(0), len);
    if (!IS_SAFE_PATHNAME(path, len, "stat"))
        XSRETURN_EMPTY;
    Zero(&q, 1, perl_krt_sqe_t);
    Zero(&st, 1, perl_krt_stat_t);
    q.op = PERL_KRT_AIO_STATX;
    q.fd = AT_FDCWD;
    q.addr = PTR2UV(path);
    q.addr2 = PTR2UV(&st);
    res = Perl_mt_aio_run(aTHX_ &q);
    if (res < 0) {
        SETERRNO((int)-res, LIB_INVARG);
        XSRETURN_EMPTY;
    }
    SP -= items;
    EXTEND(SP, 13);
    mPUSHu(st.dev);
    mPUSHu(st.ino);
    mPUSHu(st.mode);
    mPUSHu(st.nlink);
    mPUSHu(st.uid);
    mPUSHu(st.gid);
    mPUSHu(st.rdev);
    mPUSHu(st.size);
    mPUSHi(st.atime_ns / 1000000000);
    mPUSHi(st.mtime_ns / 1000000000);
    mPUSHi(st.ctime_ns / 1000000000);
    mPUSHu(st.blksize);
    mPUSHu(st.blocks);
    PUTBACK;
    return;
}

/* MT::IO::splice($in, $out, $len): moves up to $len bytes from one handle's
 * descriptor to the other's, in the kernel where it can.  Both buffers are
 * flushed first; a read-ahead in $in is not seen by the splice.  Returns the
 * bytes moved, or undef with $! set. */
XS(XS_MT_IO_splice)
{
    dXSARGS;
    IO *in_io, *out_io;
    PerlIO *in, *out;
    IV len;
    perl_krt_sqe_t q;
    IV res;
    if (items != 3)
        croak_xs_usage(cv, "in, out, len");
    in_io = sv_2io(ST(0));
    out_io = sv_2io(ST(1));
    in = IoIFP(in_io);
    out = IoOFP(out_io);
    len = SvIV(ST(2));
    if (!in || !out || len < 0)
        XSRETURN_UNDEF;
    (void)PerlIO_flush(in);
    (void)PerlIO_flush(out);
    Zero(&q, 1, perl_krt_sqe_t);
    q.op = PERL_KRT_AIO_SPLICE;
    q.fd = PerlIO_fileno(in);
    q.fd2 = PerlIO_fileno(out);
    q.len = (uint32_t)len;
    q.off = (uint64_t)-1;
    q.off2 = (uint64_t)-1;
    res = Perl_mt_aio_run(aTHX_ &q);
    if (res < 0) {
        SETERRNO((int)-res, LIB_INVARG);
        XSRETURN_UNDEF;
    }
    ST(0) = sv_2mortal(newSViv(res));
    XSRETURN(1);
}

/* MT::IO::fsync($fh): true when the descriptor is flushed to the disk. */
XS(XS_MT_IO_fsync)
{
    dXSARGS;
    IO *io;
    PerlIO *pio;
    perl_krt_sqe_t q;
    IV res;
    if (items != 1)
        croak_xs_usage(cv, "fh");
    io = sv_2io(ST(0));
    pio = IoOFP(io) ? IoOFP(io) : IoIFP(io);
    if (!pio)
        XSRETURN_NO;
    (void)PerlIO_flush(pio);
    Zero(&q, 1, perl_krt_sqe_t);
    q.op = PERL_KRT_AIO_FSYNC;
    q.fd = PerlIO_fileno(pio);
    res = Perl_mt_aio_run(aTHX_ &q);
    if (res < 0) {
        SETERRNO((int)-res, LIB_INVARG);
        XSRETURN_NO;
    }
    XSRETURN_YES;
}

/* MT::IO::aio_stats(): the counters of this interpreter's ring, as pairs. */
XS(XS_MT_IO_aio_stats)
{
    dXSARGS;
    PERL_UNUSED_VAR(items);
    SP -= items;
    EXTEND(SP, 14);
    mPUSHs(newSVpvs("submitted"));
    mPUSHu(PL_mt_aio_nsubmit);
    mPUSHs(newSVpvs("completed"));
    mPUSHu(PL_mt_aio_ndone);
    mPUSHs(newSVpvs("parked"));
    mPUSHu(PL_mt_aio_nparked);
    mPUSHs(newSVpvs("drained"));
    mPUSHu(PL_mt_aio_nsync);
    mPUSHs(newSVpvs("buffers"));
    mPUSHu(PERL_MT_AIO_BUFS - S_bufs_free(aTHX));
    mPUSHs(newSVpvs("pending"));
    mPUSHu(PL_mt_aio_pending);
    PUTBACK;
    return;
}

/* Registers the MT::IO XSUBs.  Called when a worker boots (mt_worker.c). */
void
Perl_mt_boot_io(pTHX)
{
    newXS_flags("MT::IO::stat",       XS_MT_IO_stat,       __FILE__, "$", 0);
    newXS_flags("MT::IO::splice",     XS_MT_IO_splice,     __FILE__, "$$$", 0);
    newXS_flags("MT::IO::fsync",      XS_MT_IO_fsync,      __FILE__, "$", 0);
    newXS_flags("MT::IO::aio_stats",  XS_MT_IO_aio_stats,  __FILE__, "", 0);
}

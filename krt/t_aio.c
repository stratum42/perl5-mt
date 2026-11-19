/* aio: file open/write/fsync/read/stat/close, registered buffer + files,
 * sockets, splice, timeouts. */
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include "krt.h"
#include "krt_test.h"

static int wait_one(perl_krt_aio_t *ctx, uint64_t ud, perl_krt_cqe_t *out)
{
    perl_krt_cqe_t c[8];
    int tries = 0;
    while (tries++ < 200) {
        int n = perl_krt_aio_drain(ctx, c, 8, 500000000ull), i;
        if (n < 0) return n;
        for (i = 0; i < n; i++)
            if (c[i].user_data == ud) { *out = c[i]; return 0; }
    }
    return -ETIMEDOUT;
}

int main(void)
{
    perl_krt_shm_t buf;
    perl_krt_aio_t ctx;
    perl_krt_sqe_t q;
    perl_krt_cqe_t c;
    perl_krt_stat_t st;
    char path[] = "/tmp/krt_aio_XXXXXX";
    int tfd, fd, rc, sv[2];
    const char *msg = "perl5-mt async io over krt\n";
    size_t mlen = strlen(msg);

    tfd = mkstemp(path);
    ok(tfd >= 0, "temp file");
    close(tfd);

    rc = perl_krt_shm_create(&buf, 1 << 20, 0);
    is_int(rc, 0, "registered buffer segment");
    rc = perl_krt_aio_init(&ctx, 32, &buf);
    if (rc == PERL_KRT_ENOSYS) { unlink(path); skip_all("aio not available on %s", perl_krt_platform()); }
    is_int(rc, 0, "aio_init depth 32 (%s)", perl_krt_platform());

    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_OPENAT; q.fd = AT_FDCWD; q.addr = (uint64_t)(uintptr_t)path;
    q.open_flags = O_RDWR; q.mode = 0600; q.user_data = 1;
    is_int(perl_krt_aio_submit(&ctx, &q, 1), 1, "submit OPENAT");
    is_int(wait_one(&ctx, 1, &c), 0, "OPENAT completed");
    ok(c.res >= 0, "OPENAT returned fd %d", c.res);
    fd = c.res;

    memcpy(buf.base, msg, mlen);
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_WRITE; q.fd = fd; q.flags = PERL_KRT_SQE_FIXED_BUF;
    q.addr = 0; q.len = (uint32_t)mlen; q.off = 0; q.user_data = 2;
    is_int(perl_krt_aio_submit(&ctx, &q, 1), 1, "submit WRITE (fixed buf)");
    is_int(wait_one(&ctx, 2, &c), 0, "WRITE completed");
    is_int(c.res, (int)mlen, "wrote %zu bytes", mlen);

    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_FSYNC; q.fd = fd; q.user_data = 3;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 3, &c), 0, "FSYNC completed");
    is_int(c.res, 0, "fsync ok");

    memset((char *)buf.base + 4096, 0, 256);
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_READ; q.fd = fd; q.flags = PERL_KRT_SQE_FIXED_BUF;
    q.addr = 4096; q.len = 256; q.off = 0; q.user_data = 4;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 4, &c), 0, "READ completed");
    is_int(c.res, (int)mlen, "read back %zu bytes", mlen);
    ok(memcmp((char *)buf.base + 4096, msg, mlen) == 0, "contents match");

    memset(&st, 0, sizeof st);
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_STATX; q.fd = AT_FDCWD; q.addr = (uint64_t)(uintptr_t)path;
    q.addr2 = (uint64_t)(uintptr_t)&st; q.user_data = 5;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 5, &c), 0, "STATX completed");
    is_int(c.res, 0, "stat ok");
    is_int(st.size, (long long)mlen, "stat size");
    ok(st.mtime_ns > 0 && (st.mode & 0170000) == 0100000, "regular file, mtime set");

    /* registered file index */
    rc = perl_krt_aio_register_files(&ctx, &fd, 1);
    is_int(rc, 0, "register_files");
    memset((char *)buf.base + 8192, 0, 256);
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_READ; q.fd = 0; q.flags = PERL_KRT_SQE_FIXED_BUF | PERL_KRT_SQE_FIXED_FILE;
    q.addr = 8192; q.len = 256; q.off = 0; q.user_data = 6;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 6, &c), 0, "READ via fixed file completed");
    is_int(c.res, (int)mlen, "fixed-file read size");

    /* sockets */
    ok(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    {
        struct timeval tv = { 2, 0 };
        setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_SEND_ZC; q.fd = sv[0]; q.flags = PERL_KRT_SQE_FIXED_BUF;
    q.addr = 0; q.len = (uint32_t)mlen; q.user_data = 7;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 7, &c), 0, "SEND_ZC completed (buffer reusable)");
    is_int(c.res, (int)mlen, "sent %zu bytes", mlen);
    {
        char rb[128] = {0};
        ok(read(sv[1], rb, sizeof rb) == (ssize_t)mlen && memcmp(rb, msg, mlen) == 0,
           "peer received the bytes");
    }
    /* splice file -> socket */
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_SPLICE; q.fd = fd; q.off = 0; q.fd2 = sv[0]; q.off2 = (uint64_t)-1;
    q.len = (uint32_t)mlen; q.user_data = 8;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 8, &c), 0, "SPLICE completed");
    is_int(c.res, (int)mlen, "spliced %zu bytes", mlen);
    {
        char rb[128] = {0};
        ok(read(sv[1], rb, sizeof rb) == (ssize_t)mlen && memcmp(rb, msg, mlen) == 0,
           "spliced bytes arrived");
    }

    /* batch submit + drain many */
    {
        perl_krt_sqe_t qs[8];
        perl_krt_cqe_t cs[8];
        int i, got = 0, tries = 0;
        for (i = 0; i < 8; i++) { memset(&qs[i], 0, sizeof qs[i]); qs[i].op = PERL_KRT_AIO_NOP; qs[i].user_data = 100 + i; }
        is_int(perl_krt_aio_submit(&ctx, qs, 8), 8, "batch submit 8 NOPs");
        while (got < 8 && tries++ < 50) {
            int n = perl_krt_aio_drain(&ctx, cs, 8, 200000000ull);
            if (n < 0) break;
            got += n;
        }
        is_int(got, 8, "drained 8 completions");
    }

    /* error completion */
    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_READ; q.fd = 9999; q.addr = (uint64_t)(uintptr_t)buf.base; q.len = 16; q.user_data = 9;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 9, &c), 0, "bad fd READ completed");
    is_int(c.res, -EBADF, "res = -EBADF");

    /* timeout with nothing pending */
    {
        uint64_t t0 = perl_krt_now_ns();
        int n = perl_krt_aio_drain(&ctx, &c, 1, 30000000ull);
        uint64_t dt = perl_krt_now_ns() - t0;
        is_int(n, 0, "drain with nothing pending returns 0");
        ok(dt >= 20000000ull && dt < 2000000000ull, "and waited about the timeout (%.1f ms)", dt / 1e6);
        is_int(perl_krt_aio_drain(&ctx, &c, 1, 0), 0, "non-blocking drain returns 0");
    }

    /* ready and wait do not take completions; the drain does */
    {
        uint64_t t0;
        int n;
        ok(!perl_krt_aio_ready(&ctx), "nothing ready with nothing pending");
        is_int(perl_krt_aio_wait(&ctx, 0), 0, "wait with a zero timeout returns at once");
        memset(&q, 0, sizeof q);
        q.op = PERL_KRT_AIO_NOP; q.user_data = 11;
        is_int(perl_krt_aio_submit(&ctx, &q, 1), 1, "NOP submitted");
        t0 = perl_krt_now_ns();
        is_int(perl_krt_aio_wait(&ctx, 2000000000ull), 0, "wait returns once the NOP completes");
        ok(perl_krt_now_ns() - t0 < 1000000000ull, "and did not sit out the timeout");
        ok(perl_krt_aio_ready(&ctx), "the completion is ready");
        ok(perl_krt_aio_ready(&ctx), "ready does not take it");
        n = perl_krt_aio_drain(&ctx, &c, 1, 0);
        is_int(n, 1, "the drain takes the completion");
        is_int((int)c.user_data, 11, "and it is the NOP");
        ok(!perl_krt_aio_ready(&ctx), "nothing ready after the drain");
    }

    memset(&q, 0, sizeof q);
    q.op = PERL_KRT_AIO_CLOSE; q.fd = fd; q.user_data = 10;
    perl_krt_aio_submit(&ctx, &q, 1);
    is_int(wait_one(&ctx, 10, &c), 0, "CLOSE completed");
    is_int(c.res, 0, "close ok");

    is_int(perl_krt_aio_close(&ctx), 0, "aio_close");
    perl_krt_shm_destroy(&buf);
    close(sv[0]); close(sv[1]);
    unlink(path);
    done_testing();
}

/* shm: create, alias, seal, fault on write, read-only remap. */
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "krt.h"
#include "krt_test.h"

#include <sys/wait.h>

static void on_fault_exit(int sig) { (void)sig; _exit(77); }

/* Probe the write in a child: sanitizer runtimes cannot longjmp out of a
 * fault handler, but a child that dies (or exits 77 from its handler)
 * proves the page is unwritable just as well. */
static int write_faults(volatile unsigned char *p)
{
    pid_t pid = fork();
    int st = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fault_exit;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        p[5] = 1;                 /* must fault */
        _exit(0);
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    if (WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS))
        return 1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 77)
        return 1;
    return 0;
}

int main(void)
{
    perl_krt_shm_t shm;
    void *alias = NULL, *ro = NULL;
    int rc;
    size_t i;
    unsigned char *p;

    rc = perl_krt_shm_create(&shm, 64 * 1024, 0);
    if (rc == PERL_KRT_ENOSYS)
        skip_all("shm not implemented on %s", perl_krt_platform());
    is_int(rc, 0, "shm_create");
    ok(shm.base != NULL && shm.len >= 64 * 1024, "mapped %zu bytes", shm.len);
    ok(((uintptr_t)shm.base % perl_krt_page_size()) == 0, "page aligned");

    p = (unsigned char *)shm.base;
    for (i = 0; i < shm.len; i++) p[i] = (unsigned char)(i * 7u);

    rc = perl_krt_shm_remap(&shm, &alias, PROT_READ | PROT_WRITE);
    is_int(rc, 0, "remap rw alias");
    ok(alias && alias != shm.base, "alias has a distinct address");
    ok(alias && memcmp(alias, shm.base, shm.len) == 0, "alias sees the same bytes");
    if (alias) {
        ((unsigned char *)alias)[100] = 0xAB;
        is_int(p[100], 0xAB, "write through alias visible at base");
        perl_krt_shm_unmap(alias, shm.len);
    }

    rc = perl_krt_shm_seal_ro(&shm, shm.base, shm.len);
    is_int(rc, 0, "seal_ro whole segment");
    ok(shm.flags & PERL_KRT_SHM_SEALED, "SEALED flag set");
    is_int(p[100], 0xAB, "contents preserved across seal");
    ok(p[1] == (unsigned char)7u && p[shm.len - 1] == (unsigned char)((shm.len - 1) * 7u),
       "pattern intact after seal");

    is_int(write_faults(p), 1, "write to sealed segment faults");

    rc = perl_krt_shm_remap(&shm, &ro, PROT_READ);
    is_int(rc, 0, "read-only remap after seal");
    ok(ro && memcmp(ro, shm.base, shm.len) == 0, "ro remap matches");
    if (ro) perl_krt_shm_unmap(ro, shm.len);

    rc = perl_krt_shm_remap(&shm, &alias, PROT_READ | PROT_WRITE);
    is_int(rc, -EPERM, "writable remap refused after seal");

#if defined(__linux__)
    {
        int seals = fcntl(shm.fd, F_GET_SEALS);
        ok(seals >= 0 && (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)),
           "memfd carries a write seal (0x%x)", seals);
        ok(mprotect(shm.base, shm.len, PROT_READ | PROT_WRITE) < 0,
           "mprotect(PROT_WRITE) on the sealed mapping is refused");
    }
#elif defined(__APPLE__)
    ok(mprotect(shm.base, shm.len, PROT_READ | PROT_WRITE) < 0,
       "mprotect(PROT_WRITE) refused: maximum protection lowered");
#endif

    is_int(perl_krt_shm_destroy(&shm), 0, "destroy");
    done_testing();
}

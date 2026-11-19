/* keys: alloc, bind a segment, fault under NOWRITE, re-arm after a
 * siglongjmp out of the fault handler, ENOSYS skip where unsupported. */
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "krt.h"
#include "krt_test.h"

static void on_fault_exit(int sig) { (void)sig; _exit(77); }

/* Probe a write in a child (sanitizer runtimes cannot longjmp out of a
 * fault handler): the key state is inherited across fork. */
static int write_faults(perl_krt_key_t key, uint32_t arm, volatile unsigned char *p)
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
        perl_krt_key_arm(key, arm);
        p[5] = 1;
        _exit(0);
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    if (WIFSIGNALED(st) && (WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS))
        return 1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 77)
        return 1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return 0;
    return -1;
}

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
static sigjmp_buf jb;
static void on_fault_jump(int sig) { (void)sig; siglongjmp(jb, 1); }

/* The x86 kernel resets the key register to "all allocated keys
 * access-disabled" when a signal handler runs; a siglongjmp skips
 * sigreturn, so the state after the jump is whatever the handler left,
 * and reading the image would fault until it is re-armed.  POWER keeps
 * the AMR.  Either way, re-arming NOWRITE after the jump must leave the
 * segment readable and unwritable. */
static int rearm_after_longjmp(perl_krt_key_t key, volatile unsigned char *p)
{
    pid_t pid = fork();
    int st = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        struct sigaction sa;
        unsigned char seen;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fault_jump;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        perl_krt_key_arm(key, PERL_KRT_KEY_NOWRITE);
        if (sigsetjmp(jb, 1) == 0) {
            p[6] = 2;           /* faults, handler jumps back */
            _exit(3);           /* no fault: wrong */
        }
        /* back from the handler: re-arm, then the image must be readable */
        perl_krt_key_arm(key, PERL_KRT_KEY_NOWRITE);
        seen = p[6];
        if (seen != (unsigned char)(6 * 7u))
            _exit(4);
        /* ... and still unwritable: a second fault exits 77 */
        sa.sa_handler = on_fault_exit;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        p[7] = 3;
        _exit(5);
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 100 + WTERMSIG(st);
    return -1;
}
#endif

int main(void)
{
    perl_krt_shm_t shm;
    perl_krt_key_t key;
    int rc;
    size_t i;
    unsigned char *p;
    void *ro = NULL;

    rc = perl_krt_key_alloc(&key);
    if (rc == PERL_KRT_ENOSYS)
        skip_all("protection keys not available on %s", perl_krt_platform());
    is_int(rc, 0, "key_alloc");
    ok(key >= 0, "key %d", key);

    rc = perl_krt_shm_create(&shm, 64 * 1024, 0);
    is_int(rc, 0, "shm_create");
    p = (unsigned char *)shm.base;
    for (i = 0; i < shm.len; i++) p[i] = (unsigned char)(i * 7u);

    rc = perl_krt_shm_bind_key(&shm, key);
    is_int(rc, 0, "bind the segment to the key");
    rc = perl_krt_shm_seal_size(&shm);
    is_int(rc, 0, "size seals and read-only alias");

    is_int(perl_krt_key_arm(key, 0), 0, "arm read/write");
    p[1] = 9;
    is_int(p[1], 9, "writable while armed read/write");
    p[1] = (unsigned char)7u;

    is_int(write_faults(key, PERL_KRT_KEY_NOWRITE, p), 1,
           "a write under NOWRITE faults");
    is_int(write_faults(key, 0, p), 0,
           "a write with the key armed read/write does not");

    is_int(perl_krt_key_arm(key, PERL_KRT_KEY_NOWRITE), 0, "arm NOWRITE here");
    is_int(p[2], (unsigned char)(2 * 7u), "readable under NOWRITE");
    is_int(p[shm.len - 1], (unsigned char)((shm.len - 1) * 7u), "last byte readable");

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    is_int(rearm_after_longjmp(key, p), 77,
           "re-armed after a siglongjmp out of the fault handler: readable, "
           "still unwritable");
#else
    ok(1, "siglongjmp case skipped under a sanitizer");
#endif

    rc = perl_krt_shm_remap(&shm, &ro, PROT_READ);
    is_int(rc, 0, "read-only remap through the alias");
    ok(ro && ((unsigned char *)ro)[3] == (unsigned char)(3 * 7u),
       "alias sees the data");
    if (ro) perl_krt_shm_unmap(ro, shm.len);

    is_int(perl_krt_key_arm(key, 0), 0, "disarm");
    is_int(perl_krt_shm_destroy(&shm), 0, "shm_destroy");
    is_int(perl_krt_key_free(key), 0, "key_free");
    done_testing();
}

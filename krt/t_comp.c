/* compartments: no shared VM, shared shm, inherited fds, control socket,
 * exit status, kill/reap. */
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "krt.h"
#include "krt_test.h"

static int global_counter = 1;
typedef struct { perl_krt_shm_t *shm; int pipe_rd; } arg_t;

static int child_fn(void *a)
{
    arg_t *arg = (arg_t *)a;
    char buf[16] = {0};
    global_counter = 42;                         /* private copy only */
    memcpy(arg->shm->base, "hello from comp", 16); /* shared */
    if (read(arg->pipe_rd, buf, 4) != 4) return 3;
    if (memcmp(buf, "ping", 4) != 0) return 4;
    if (perl_krt_comp_ctl_fd() < 0) return 6;
    if (write(perl_krt_comp_ctl_fd(), "pong", 4) != 4) return 5;
    return 7;
}

static int sleeper_fn(void *a) { (void)a; for (;;) pause(); return 0; }

int main(void)
{
    perl_krt_shm_t shm;
    perl_krt_comp_t comp;
    arg_t arg;
    int pfd[2], rc, status = -1;
    char buf[8] = {0};

    rc = perl_krt_shm_create(&shm, 4096, 0);
    if (rc == PERL_KRT_ENOSYS)
        skip_all("compartments not implemented on %s", perl_krt_platform());
    is_int(rc, 0, "shm for the compartment");
    memset(shm.base, 0, 64);
    ok(pipe(pfd) == 0, "pipe");
    arg.shm = &shm; arg.pipe_rd = pfd[0];
    is_int(perl_krt_comp_ctl_fd(), -1, "ctl_fd is -1 in the parent");

    rc = perl_krt_comp_spawn(&comp, child_fn, &arg, &shm, 1);
    is_int(rc, 0, "comp_spawn");
    ok(comp.pid > 0 && comp.ctl_fd >= 0, "pid %d ctl_fd %d", (int)comp.pid, comp.ctl_fd);
    is_int(perl_krt_comp_reap_flags(&comp, &status, PERL_KRT_REAP_NOHANG), -EAGAIN,
           "reap NOHANG while running");
    ok(write(pfd[1], "ping", 4) == 4, "write ping into the inherited pipe");
    ok(read(comp.ctl_fd, buf, 4) == 4 && memcmp(buf, "pong", 4) == 0,
       "child answered on the control socket");
    is_int(perl_krt_comp_reap(&comp, &status), 0, "reap");
    is_int(status, 7, "exit status propagated");
    is_int(global_counter, 1, "parent .data untouched (no CLONE_VM)");
    ok(memcmp(shm.base, "hello from comp", 16) == 0, "shm write visible in parent");
    is_int(comp.ctl_fd, -1, "ctl_fd closed by reap");

    rc = perl_krt_comp_spawn(&comp, sleeper_fn, NULL, NULL, 0);
    is_int(rc, 0, "spawn a sleeper");
    is_int(perl_krt_comp_kill(&comp, SIGKILL), 0, "kill it");
    is_int(perl_krt_comp_reap(&comp, &status), 0, "reap killed child");
    is_int(status, 128 + SIGKILL, "128+signal status");

#if defined(__linux__)
    {
        /* shared descriptor table variant: a child-opened fd is visible here */
        char path[] = "/tmp/krt_comp_XXXXXX";
        int tfd = mkstemp(path);
        close(tfd);
        unlink(path);
        rc = perl_krt_comp_spawn_flags(&comp, child_fn, &arg, &shm, 1,
                                       PERL_KRT_COMP_SHARE_FILES);
        is_int(rc, 0, "comp_spawn SHARE_FILES");
        ok(write(pfd[1], "ping", 4) == 4, "ping (shared table)");
        ok(read(comp.ctl_fd, buf, 4) == 4, "pong (shared table)");
        is_int(perl_krt_comp_reap(&comp, &status), 0, "reap (shared table)");
        is_int(status, 7, "status (shared table)");
        is_int(global_counter, 1, "parent .data still untouched");
    }
#endif
    perl_krt_shm_destroy(&shm);
    done_testing();
}

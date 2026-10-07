#include "libc.h"

static void print_str(const char* s) {
    if (s) write(1, s, strlen(s));
}

static void assert_true(int cond, const char* msg) {
    if (!cond) {
        print_str("FAIL: ");
        print_str(msg);
        print_str("\n");
        exit(1);
    }
    print_str("PASS: ");
    print_str(msg);
    print_str("\n");
}

static volatile int tt_received = 0;
static void handle_ttin(int sig) {
    (void) sig;
    tt_received = 1;
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    print_str("=== RUNNING JOB CONTROL & PROCESS GROUP TESTS ===\n");

    /* Test 1: getpgrp, getpgid, getppid */
    int my_pid = getpid();
    int my_pgid = getpgrp();
    assert_true(my_pgid > 0, "getpgrp() returned valid pgid");
    assert_true(getpgid(my_pid) == my_pgid, "getpgid(pid) matches getpgrp()");
    assert_true(getppid() > 0, "getppid() returned valid parent pid");

    /* Test 2: setpgid child into new process group */
    int child = fork();
    if (child == 0) {
        setpgid(0, getpid());
        if (getpgrp() != getpid()) {
            exit(2);
        }
        exit(0);
    }
    int st = 0;
    waitpid(child, &st, 0);
    assert_true(WIFEXITED(st) && WEXITSTATUS(st) == 0, "setpgid in child creates new pgid");

    /* Test 3: setsid and getsid */
    child = fork();
    if (child == 0) {
        int sid = setsid();
        if (sid != getpid()) exit(2);
        if (getsid(0) != sid) exit(3);
        if (getpgrp() != sid) exit(4);
        exit(0);
    }
    waitpid(child, &st, 0);
    assert_true(WIFEXITED(st) && WEXITSTATUS(st) == 0, "setsid creates new session and pgid");

    /* Test 4: tcgetpgrp and tcsetpgrp */
    int orig_fg = tcgetpgrp(0);
    assert_true(orig_fg > 0, "tcgetpgrp(0) returns valid foreground pgid");

    /* Test 5: Background process read from TTY generates SIGTTIN */
    signal(SIGTTIN, handle_ttin);
    child = fork();
    if (child == 0) {
        /* Put into separate background process group */
        setpgid(0, getpid());
        signal(SIGTTIN, handle_ttin);
        char ch;
        int r = read(0, &ch, 1);
        if (tt_received && r < 0) {
            exit(0);
        }
        exit(1);
    }
    waitpid(child, &st, 0);
    assert_true(WIFEXITED(st) && WEXITSTATUS(st) == 0, "background read generates SIGTTIN");

    print_str("=== ALL JOBTESTS PASSED ===\n");
    return 0;
}

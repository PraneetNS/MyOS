#include "libc.h"

static void print_str(const char* s) {
    if (s) write(1, s, strlen(s));
}

static volatile int usr1_received = 0;
static volatile int usr2_received = 0;
static volatile int alrm_received = 0;
static volatile int pipe_received = 0;

static void handle_usr1(int sig) {
    (void) sig;
    usr1_received = 1;
}

static void handle_usr2(int sig) {
    (void) sig;
    usr2_received = 1;
}

static void handle_alrm(int sig) {
    (void) sig;
    alrm_received = 1;
}

static void handle_pipe(int sig) {
    (void) sig;
    pipe_received = 1;
}

int main(int argc, char** argv) {
    (void) argc;
    (void) argv;

    print_str("=== RUNNING SIGTEST ===\n");

    /* Test 1: Simple signal delivery and handler */
    signal(SIGUSR1, handle_usr1);
    kill(getpid(), SIGUSR1);
    if (!usr1_received) {
        print_str("FAIL: SIGUSR1 handler not executed\n");
        return 1;
    }
    print_str("PASS: SIGUSR1 delivery and return\n");

    /* Test 2: sigprocmask block and unblock */
    signal(SIGUSR2, handle_usr2);
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, &oldset);

    kill(getpid(), SIGUSR2);
    if (usr2_received) {
        print_str("FAIL: SIGUSR2 delivered while blocked\n");
        return 1;
    }

    sigprocmask(SIG_UNBLOCK, &set, 0);
    if (!usr2_received) {
        print_str("FAIL: SIGUSR2 not delivered upon unblock\n");
        return 1;
    }
    print_str("PASS: sigprocmask blocking and deferred delivery\n");

    /* Test 3: Alarm and pause */
    signal(SIGALRM, handle_alrm);
    alarm(1);
    print_str("Waiting for alarm(1)...\n");
    pause();
    if (!alrm_received) {
        print_str("FAIL: SIGALRM not received after pause\n");
        return 1;
    }
    print_str("PASS: alarm and pause\n");

    /* Test 4: Broken pipe produces SIGPIPE */
    signal(SIGPIPE, handle_pipe);
    int pfd[2];
    if (pipe(pfd) == 0) {
        close(pfd[0]); /* Close read end */
        char buf[4] = "abc";
        int w = write(pfd[1], buf, 3);
        close(pfd[1]);
        if (!pipe_received || w >= 0) {
            print_str("FAIL: write on broken pipe did not trigger SIGPIPE\n");
            return 1;
        }
        print_str("PASS: SIGPIPE generated on broken pipe\n");
    }

    /* Test 5: Child fault (SIGSEGV) reported to parent via wait status */
    int child = fork();
    if (child == 0) {
        /* Cause null pointer dereference */
        volatile int* bad = (volatile int*) 0x0;
        *bad = 123;
        exit(0);
    } else {
        int st = 0;
        waitpid(child, &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV) {
            print_str("PASS: child fault generated SIGSEGV and status\n");
        } else {
            print_str("FAIL: child status was not SIGSEGV\n");
            return 1;
        }
    }

    /* Test 6: SIGSTOP and SIGCONT with WUNTRACED */
    child = fork();
    if (child == 0) {
        while (1) {
            sys_ticks();
        }
        exit(0);
    } else {
        /* Stop child */
        kill(child, SIGSTOP);
        int st = 0;
        int wp = waitpid(child, &st, WUNTRACED);
        if (wp == child && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP) {
            print_str("PASS: child stopped and reported via WUNTRACED\n");
        } else {
            print_str("FAIL: WUNTRACED failed to report SIGSTOP\n");
            kill(child, SIGKILL);
            return 1;
        }

        /* Resume child */
        kill(child, SIGCONT);

        /* Kill child */
        kill(child, SIGTERM);
        waitpid(child, &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM) {
            print_str("PASS: child terminated by SIGTERM\n");
        } else {
            print_str("FAIL: child not reported terminated by SIGTERM\n");
            return 1;
        }
    }

    /* Test 7: nanosleep interrupted by signal returns -EINTR with remaining time > 0 */
    usr1_received = 0;
    signal(SIGUSR1, handle_usr1);
    child = fork();
    if (child == 0) {
        struct timespec req = { 5, 0 };
        struct timespec rem = { 0, 0 };
        int ret = nanosleep(&req, &rem);
        if ((ret == -EINTR || (ret == -1 && errno == EINTR)) && rem.tv_sec > 0 && usr1_received) {
            print_str("PASS: nanosleep interrupted returned -EINTR with remaining time > 0\n");
            exit(0);
        } else {
            print_str("FAIL: nanosleep did not return -EINTR with remaining time\n");
            exit(1);
        }
    } else {
        usleep(50000);
        kill(child, SIGUSR1);
        int st = 0;
        waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            return 1;
        }
    }

    print_str("=== ALL SIGTESTS PASSED ===\n");
    return 0;
}

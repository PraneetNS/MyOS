/* consumer.c -- demonstrates pipe() and fork(): parent creates a pipe,
   forks a producer child, and reads messages from the pipe. */

#include "libc.h"

static void delay(void) {
    for (volatile unsigned int i = 0; i < 6000000; i++) { }
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    int fds[2];
    if (pipe(fds) < 0) {
        const char* err = "[consumer] pipe failed\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    }

    int pid = fork();
    if (pid < 0) {
        const char* err = "[consumer] fork failed\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    }

    if (pid == 0) {
        /* Child: producer */
        close(fds[0]); /* close read end */
        const char* messages[3] = {
            "message 1 from child producer\n",
            "message 2 from child producer\n",
            "message 3 from child producer\n"
        };
        for (int i = 0; i < 3; i++) {
            delay();
            const char* m = messages[i];
            write(fds[1], m, strlen(m));
        }
        close(fds[1]); /* signals EOF to consumer */
        sys_exit();
    } else {
        /* Parent: consumer */
        close(fds[1]); /* close write end */
        const char* m1 = "[consumer] pid ";
        const char* m2 = " starting, will read from pipe (blocking).\n";
        sys_write(1, m1, strlen(m1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, m2, strlen(m2));

        char buf[64];
        for (;;) {
            int n = read(fds[0], buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            const char* mg = "[consumer] got: ";
            sys_write(1, mg, strlen(mg));
            sys_write(1, buf, strlen(buf));
        }

        close(fds[0]);
        waitpid(pid, 0, 0);
        const char* md = "[consumer] done, exiting.\n";
        sys_write(1, md, strlen(md));
        sys_exit();
    }

    return 0;
}

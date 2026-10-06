/* producer.c -- demonstrates pipe() and fork(): parent creates a pipe,
   forks a consumer child, and writes messages to the pipe. */

#include "libc.h"

static void delay(void) {
    for (volatile unsigned int i = 0; i < 6000000; i++) { }
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    int fds[2];
    if (pipe(fds) < 0) {
        const char* err = "[producer] pipe failed\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    }

    int pid = fork();
    if (pid < 0) {
        const char* err = "[producer] fork failed\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    }

    if (pid == 0) {
        /* Child: consumer */
        close(fds[1]); /* close write end */
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
        const char* cd = "[consumer] done, exiting.\n";
        sys_write(1, cd, strlen(cd));
        sys_exit();
    } else {
        /* Parent: producer */
        close(fds[0]); /* close read end */
        const char* m1 = "[producer] pid ";
        const char* m2 = " starting, will write 3 messages to the pipe.\n";
        sys_write(1, m1, strlen(m1));
        print_uint((unsigned int) sys_getpid());
        sys_write(1, m2, strlen(m2));

        const char* messages[3] = {
            "first message from producer\n",
            "second message from producer\n",
            "third and final message\n"
        };

        for (int i = 0; i < 3; i++) {
            delay();
            const char* mw1 = "[producer] writing message ";
            const char* mw2 = " to pipe...\n";
            sys_write(1, mw1, strlen(mw1));
            print_uint((unsigned int) (i + 1));
            sys_write(1, mw2, strlen(mw2));
            const char* m = messages[i];
            write(fds[1], m, strlen(m));
        }

        close(fds[1]); /* signals EOF to consumer */
        wait(pid);
        const char* md = "[producer] done, exiting.\n";
        sys_write(1, md, strlen(md));
        sys_exit();
    }

    return 0;
}

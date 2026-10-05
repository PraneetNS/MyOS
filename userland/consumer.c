/* consumer.c -- reads from the global pipe via sys_pipe_read(), which
   BLOCKS (a real scheduler suspend, not a busy-wait) whenever the pipe
   is empty. Run this concurrently with producer.elf (launch consumer
   first from the shell, then producer, while consumer is still
   running) to see it genuinely wait for data rather than polling. */

#include "libc.h"

void _start(void) {
    const char* m1 = "[consumer] pid ";
    const char* m2 = " starting, will read 3 messages from the pipe (blocking).\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, m2, strlen(m2));

    for (int i = 0; i < 3; i++) {
        const char* mw1 = "[consumer] waiting for message ";
        const char* mw2 = "...\n";
        sys_write(1, mw1, strlen(mw1));
        print_uint((unsigned int) (i + 1));
        sys_write(1, mw2, strlen(mw2));

        char buf[64];
        unsigned int n = sys_pipe_read(buf, sizeof(buf) - 1);
        buf[n] = '\0';

        const char* mg = "[consumer] got: ";
        sys_write(1, mg, strlen(mg));
        sys_write(1, buf, strlen(buf));
    }

    const char* md = "[consumer] done, exiting.\n";
    sys_write(1, md, strlen(md));
    sys_exit();

    for (;;) { }
}

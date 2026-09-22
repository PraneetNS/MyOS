/* consumer.c -- reads from the global pipe via sys_pipe_read(), which
   BLOCKS (a real scheduler suspend, not a busy-wait) whenever the pipe
   is empty. Run this concurrently with producer.elf (launch consumer
   first from the shell, then producer, while consumer is still
   running) to see it genuinely wait for data rather than polling. */

#include "libc.h"

void _start(void) {
    sys_write("[consumer] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(" starting, will read 3 messages from the pipe (blocking).\n");

    for (int i = 0; i < 3; i++) {
        sys_write("[consumer] waiting for message ");
        print_uint((unsigned int) (i + 1));
        sys_write("...\n");

        char buf[64];
        unsigned int n = sys_pipe_read(buf, sizeof(buf) - 1);
        buf[n] = '\0';

        sys_write("[consumer] got: ");
        sys_write(buf);
    }

    sys_write("[consumer] done, exiting.\n");
    sys_exit();

    for (;;) { }
}

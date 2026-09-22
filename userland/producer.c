/* producer.c -- writes several short messages into the global pipe,
   pausing briefly between each (a busy-wait, not a real sleep syscall --
   MyOS doesn't have one yet) so a concurrently-running consumer.elf has
   to genuinely BLOCK waiting for data rather than finding it all
   already there. */

#include "libc.h"

static void delay(void) {
    for (volatile unsigned int i = 0; i < 6000000; i++) { }
}

void _start(void) {
    sys_write("[producer] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(" starting, will write 3 messages to the pipe.\n");

    const char* messages[3] = {
        "first message from producer\n",
        "second message from producer\n",
        "third and final message\n"
    };

    for (int i = 0; i < 3; i++) {
        delay();
        sys_write("[producer] writing message ");
        print_uint((unsigned int) (i + 1));
        sys_write(" to pipe...\n");
        const char* m = messages[i];
        unsigned int len = 0;
        while (m[len]) len++;
        sys_pipe_write(m, len);
    }

    sys_write("[producer] done, exiting.\n");
    sys_exit();

    for (;;) { }
}

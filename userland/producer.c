/* producer.c -- writes several short messages into the global pipe,
   pausing briefly between each (a busy-wait, not a real sleep syscall --
   MyOS doesn't have one yet) so a concurrently-running consumer.elf has
   to genuinely BLOCK waiting for data rather than finding it all
   already there. */

#include "libc.h"

static void delay(void) {
    for (volatile unsigned int i = 0; i < 6000000; i++) { }
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;
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
        unsigned int len = 0;
        while (m[len]) len++;
        sys_pipe_write(m, len);
    }

    const char* md = "[producer] done, exiting.\n";
    sys_write(1, md, strlen(md));
    sys_exit();

    for (;;) { }
}

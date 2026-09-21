/* reader.c -- proves the SYS_OPEN/SYS_READ/SYS_CLOSE syscalls work by
   reading hello.txt entirely from userland, in small chunks, without
   any help from the shell's own (kernel-side) `cat` command. */

#include "libc.h"

void _start(void) {
    sys_write("[reader] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(": opening hello.txt via sys_open...\n");

    int fd = sys_open("hello.txt");
    if (fd < 0) {
        sys_write("[reader] open failed!\n");
        sys_exit();
    }

    sys_write("[reader] got fd ");
    print_uint((unsigned int) fd);
    sys_write(", reading in 32-byte chunks via sys_read:\n---\n");

    char buf[33];
    int n;
    int total = 0;
    while ((n = sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        sys_write(buf);
        total += n;
    }

    sys_close(fd);
    sys_write("\n---\n[reader] done, ");
    print_uint((unsigned int) total);
    sys_write(" bytes read via syscalls.\n");
    sys_exit();

    for (;;) { }
}

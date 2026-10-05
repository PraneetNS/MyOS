/* reader.c -- proves the SYS_OPEN/SYS_READ/SYS_CLOSE syscalls work by
   reading hello.txt entirely from userland, in small chunks, without
   any help from the shell's own (kernel-side) `cat` command. */

#include "libc.h"

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    const char* m1 = "[reader] pid ";
    const char* m2 = ": opening hello.txt via sys_open...\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, m2, strlen(m2));

    int fd = sys_open("hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        const char* err = "[reader] open failed!\n";
        sys_write(1, err, strlen(err));
        sys_exit();
    }

    const char* m3 = "[reader] got fd ";
    const char* m4 = ", reading in 32-byte chunks via sys_read:\n---\n";
    sys_write(1, m3, strlen(m3));
    print_uint((unsigned int) fd);
    sys_write(1, m4, strlen(m4));

    char buf[33];
    int n;
    int total = 0;
    while ((n = sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        sys_write(1, buf, (unsigned int)n);
        total += n;
    }

    sys_close(fd);
    const char* m5 = "\n---\n[reader] done, ";
    const char* m6 = " bytes read via syscalls.\n";
    sys_write(1, m5, strlen(m5));
    print_uint((unsigned int) total);
    sys_write(1, m6, strlen(m6));
    sys_exit();

    for (;;) { }
}

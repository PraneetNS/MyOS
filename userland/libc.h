#ifndef LIBC_H
#define LIBC_H

/* MyOS's syscall convention: eax = number, ebx/ecx/edx = up to 3 args,
   return value comes back in eax. See src/syscall.c for the kernel side. */

static inline void sys_write(const char* s) {
    asm volatile ("int $0x80" : : "a"(1), "b"(s));
}

static inline void sys_exit(void) {
    asm volatile ("int $0x80" : : "a"(0));
}

static inline int sys_getpid(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(2));
    return ret;
}

static inline int sys_open(const char* name) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(3), "b"(name));
    return ret;
}

static inline int sys_read(int fd, void* buf, unsigned int len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(4), "b"(fd), "c"(buf), "d"(len));
    return ret;
}

static inline void sys_close(int fd) {
    asm volatile ("int $0x80" : : "a"(5), "b"(fd));
}

/* Spawns `name` as a child process and BLOCKS until it exits -- unlike
   the shell's `run`, which is fire-and-forget. Returns the child's pid,
   or -1 if it couldn't be spawned (bad name, out of process slots, etc). */
static inline int sys_spawn_wait(const char* name) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"(name));
    return ret;
}

/* Tiny freestanding helpers -- no libc means no <stdio.h>/<string.h>. */

static inline void print_uint(unsigned int n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { sys_write("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    sys_write(&buf[i]);
}

#endif

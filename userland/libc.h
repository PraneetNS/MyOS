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

/* Classic fork() semantics: duplicates the calling process (address
   space and all). Returns the child's pid to the parent, 0 to the
   child, or -1 on failure. The syscall "returns twice" -- both
   processes resume at the instruction right after this call. */
static inline int sys_fork(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(7));
    return ret;
}

/* One global, system-wide pipe (see src/pipe.c). Write never blocks
   (drops data if the 256-byte ring buffer is full); read blocks until
   a writer provides data. */
static inline unsigned int sys_pipe_write(const void* buf, unsigned int len) {
    unsigned int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(8), "b"(buf), "c"(len));
    return ret;
}

static inline unsigned int sys_pipe_read(void* buf, unsigned int maxlen) {
    unsigned int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(9), "b"(buf), "c"(maxlen));
    return ret;
}

/* Replaces the CALLING process's own code/data with a freshly loaded
   ELF, keeping its pid/ppid/open files. Only returns (with -1) on
   failure -- on success there is no "returning": the old program's
   code is gone. This is the missing piece that makes fork() genuinely
   useful: fork() to create a new process, then exec() to turn it into
   a different program. */
static inline int sys_exec(const char* name) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(name));
    return ret;
}

/* Blocks until the process with this pid exits (or returns immediately
   if it already has, or never existed). Unlike sys_spawn_wait(), this
   doesn't spawn anything -- it's for waiting on a child you already
   have, e.g. one created via sys_fork(). */
static inline void sys_wait(int pid) {
    asm volatile ("int $0x80" : : "a"(11), "b"(pid));
}

/* Classic sbrk(): grows (or queries, with increment=0) this process's
   heap by `increment` bytes, returning the PREVIOUS break -- so the
   newly available memory is [return value, return value + increment).
   Returns (void*)-1 (cast here to a plain int -1) on failure. New pages
   are zero-initialized. */
static inline void* sys_sbrk(int increment) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(12), "b"(increment));
    return (void*) ret;
}

/* Tiny freestanding helpers -- no libc means no <stdio.h>/<string.h>. */

static inline void print_uint(unsigned int n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { sys_write("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    sys_write(&buf[i]);
}

static inline void print_hex(unsigned int v) {
    char hex[11] = "0x00000000";
    const char* digits = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    sys_write(hex);
}

#endif

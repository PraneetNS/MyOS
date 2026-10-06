#ifndef LIBC_H
#define LIBC_H

#include "../src/errno.h"

/* Open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_CLOEXEC   0x80000

#define FD_CLOEXEC  1

#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4

/* Seek constants */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* File modes */
#define S_IFMT      0xF000
#define S_IFREG     0x8000
#define S_IFDIR     0x4000
#define S_IFCHR     0x2000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

struct stat {
    unsigned int st_dev;
    unsigned int st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_size;
    unsigned int st_blksize;
    unsigned int st_blocks;
};

struct dirent {
    unsigned int d_ino;
    char d_name[64];
    unsigned int d_type; /* 1 = file, 2 = dir */
    unsigned int d_size;
};

static inline unsigned int strlen(const char* s) {
    unsigned int len = 0;
    while (s && s[len]) len++;
    return len;
}

static inline int sys_write(int fd, const void* buf, unsigned int len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(1), "b"(fd), "c"(buf), "d"(len));
    return ret;
}

static inline int write(int fd, const void* buf, unsigned int len) {
    return sys_write(fd, buf, len);
}

static inline void sys_exit(void) {
    asm volatile ("int $0x80" : : "a"(0));
}

static inline void exit(int status) {
    (void) status;
    sys_exit();
}

static inline int sys_getpid(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(2));
    return ret;
}

static inline int getpid(void) {
    return sys_getpid();
}

static inline int sys_open(const char* name, int flags, int mode) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(3), "b"(name), "c"(flags), "d"(mode));
    return ret;
}

static inline int open(const char* name, int flags, ...) {
    return sys_open(name, flags, 0);
}

static inline int sys_read(int fd, void* buf, unsigned int len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(4), "b"(fd), "c"(buf), "d"(len));
    return ret;
}

static inline int read(int fd, void* buf, unsigned int len) {
    return sys_read(fd, buf, len);
}

static inline int sys_close(int fd) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(5), "b"(fd));
    return ret;
}

static inline int close(int fd) {
    return sys_close(fd);
}

static inline int sys_spawn_wait(const char* name) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(6), "b"(name));
    return ret;
}

static inline int sys_fork(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(7));
    return ret;
}

static inline int fork(void) {
    return sys_fork();
}

static inline int sys_pipe(int fds[2]) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(8), "b"(fds));
    return ret;
}

static inline int pipe(int fds[2]) {
    return sys_pipe(fds);
}

static inline int sys_exec(const char* name, const char* const* argv) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(10), "b"(name), "c"(argv));
    return ret;
}

static inline int exec(const char* name, const char* const* argv) {
    return sys_exec(name, argv);
}

static inline void sys_wait(int pid) {
    asm volatile ("int $0x80" : : "a"(11), "b"(pid));
}

static inline void wait(int pid) {
    sys_wait(pid);
}

static inline void* sys_sbrk(int increment) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(12), "b"(increment));
    return (void*) ret;
}

static inline void* sbrk(int increment) {
    return sys_sbrk(increment);
}

static inline int sys_sync(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(13));
    return ret;
}

static inline int sync(void) {
    return sys_sync();
}

static inline int sys_lseek(int fd, int offset, int whence) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(14), "b"(fd), "c"(offset), "d"(whence));
    return ret;
}

static inline int lseek(int fd, int offset, int whence) {
    return sys_lseek(fd, offset, whence);
}

static inline int sys_stat(const char* path, struct stat* st) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(15), "b"(path), "c"(st));
    return ret;
}

static inline int stat(const char* path, struct stat* st) {
    return sys_stat(path, st);
}

static inline int sys_fstat(int fd, struct stat* st) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(16), "b"(fd), "c"(st));
    return ret;
}

static inline int fstat(int fd, struct stat* st) {
    return sys_fstat(fd, st);
}

static inline int sys_getdents(int fd, struct dirent* dirp, unsigned int count) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(17), "b"(fd), "c"(dirp), "d"(count));
    return ret;
}

static inline int getdents(int fd, struct dirent* dirp, unsigned int count) {
    return sys_getdents(fd, dirp, count);
}

static inline int sys_mkdir(const char* path, int mode) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(18), "b"(path), "c"(mode));
    return ret;
}

static inline int mkdir(const char* path, int mode) {
    return sys_mkdir(path, mode);
}

static inline int sys_rmdir(const char* path) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(19), "b"(path));
    return ret;
}

static inline int rmdir(const char* path) {
    return sys_rmdir(path);
}

static inline int sys_unlink(const char* path) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(20), "b"(path));
    return ret;
}

static inline int unlink(const char* path) {
    return sys_unlink(path);
}

static inline int sys_rename(const char* oldpath, const char* newpath) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(21), "b"(oldpath), "c"(newpath));
    return ret;
}

static inline int rename(const char* oldpath, const char* newpath) {
    return sys_rename(oldpath, newpath);
}

static inline int sys_chdir(const char* path) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(22), "b"(path));
    return ret;
}

static inline int chdir(const char* path) {
    return sys_chdir(path);
}

static inline char* sys_getcwd(char* buf, unsigned int size) {
    char* ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(23), "b"(buf), "c"(size));
    return ret;
}

static inline char* getcwd(char* buf, unsigned int size) {
    return sys_getcwd(buf, size);
}

static inline int sys_dup(int fd) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(24), "b"(fd));
    return ret;
}

static inline int dup(int fd) {
    return sys_dup(fd);
}

static inline int sys_dup2(int oldfd, int newfd) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(25), "b"(oldfd), "c"(newfd));
    return ret;
}

static inline int dup2(int oldfd, int newfd) {
    return sys_dup2(oldfd, newfd);
}

static inline int sys_fcntl(int fd, int cmd, int arg) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(26), "b"(fd), "c"(cmd), "d"(arg));
    return ret;
}

static inline int fcntl(int fd, int cmd, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    int arg = __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return sys_fcntl(fd, cmd, arg);
}

static inline void print_uint(unsigned int n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { sys_write(1, "0", 1); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    sys_write(1, &buf[i], 10 - i);
}

static inline void print_hex(unsigned int v) {
    char hex[11] = "0x00000000";
    const char* digits = "0123456789ABCDEF";
    for (int i = 9; i >= 2; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    sys_write(1, hex, 10);
}

#endif

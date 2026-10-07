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

/* Waitpid constants and macros */
#define WNOHANG        1
#define WUNTRACED      2
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((s) & 0x7f) > 0 && ((s) & 0x7f) != 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    (((s) >> 8) & 0xff)

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

static inline void sys_exit_status(int status) {
    asm volatile ("int $0x80" : : "a"(0), "b"(status));
}

static inline void sys_exit(void) {
    sys_exit_status(0);
}

static inline void exit(int status) {
    sys_exit_status(status);
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

static inline int sys_waitpid(int pid, int* status, int options) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(11), "b"(pid), "c"(status), "d"(options));
    return ret;
}

static inline int waitpid(int pid, int* status, int options) {
    return sys_waitpid(pid, status, options);
}

static inline int wait(int* status) {
    return waitpid(-1, status, 0);
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

static inline int sys_sleep(unsigned int seconds) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(27), "b"(seconds));
    return ret;
}

static inline unsigned int sleep(unsigned int seconds) {
    return (unsigned int) sys_sleep(seconds);
}

static inline unsigned int sys_ticks(void) {
    unsigned int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(28));
    return ret;
}

static inline unsigned int sys_free_frames(void) {
    unsigned int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(29));
    return ret;
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

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANON        0x20
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void*)-1)

static inline void* sys_mmap(void* addr, unsigned int len, int prot, int flags, int fd, unsigned int offset) {
    int ret;
    asm volatile (
        "pushl %6\n\t"
        "pushl %%ebp\n\t"
        "movl 4(%%esp), %%ebp\n\t"
        "int $0x80\n\t"
        "popl %%ebp\n\t"
        "addl $4, %%esp\n\t"
        : "=a"(ret)
        : "a"(30), "b"(addr), "c"(len), "d"(prot), "S"(flags), "m"(offset), "D"(fd)
        : "memory"
    );
    return (void*) ret;
}

static inline void* mmap(void* addr, unsigned int len, int prot, int flags, int fd, unsigned int offset) {
    return sys_mmap(addr, len, prot, flags, fd, offset);
}

static inline int sys_munmap(void* addr, unsigned int len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(31), "b"(addr), "c"(len));
    return ret;
}

static inline int munmap(void* addr, unsigned int len) {
    return sys_munmap(addr, len);
}

static inline int sys_mprotect(void* addr, unsigned int len, int prot) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(32), "b"(addr), "c"(len), "d"(prot));
    return ret;
}

static inline int mprotect(void* addr, unsigned int len, int prot) {
    return sys_mprotect(addr, len, prot);
}

#define MMAP_THRESHOLD  65536

typedef struct malloc_chunk {
    unsigned int size;           /* Usable payload size in bytes */
    unsigned int total_size;     /* Total allocation size including header, or mmap size */
    unsigned int is_mmap;        /* 1 if mmap'd, 0 if sbrk-allocated */
    struct malloc_chunk* next;   /* Next block in free list (when free) */
} malloc_chunk_t;

static malloc_chunk_t* __libc_free_list = 0;

static inline void* malloc(unsigned int size) {
    if (size == 0) return 0;

    size = (size + 7) & ~7u;

    if (size >= MMAP_THRESHOLD) {
        unsigned int total = sizeof(malloc_chunk_t) + size;
        total = (total + 4095) & ~4095u;
        void* p = mmap(0, total, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) return 0;
        malloc_chunk_t* chunk = (malloc_chunk_t*) p;
        chunk->size = size;
        chunk->total_size = total;
        chunk->is_mmap = 1;
        chunk->next = 0;
        return (void*)(chunk + 1);
    }

    malloc_chunk_t* prev = 0;
    malloc_chunk_t* curr = __libc_free_list;

    while (curr) {
        if (curr->size >= size) {
            if (curr->size >= size + sizeof(malloc_chunk_t) + 16) {
                unsigned int orig_size = curr->size;
                curr->size = size;
                malloc_chunk_t* split = (malloc_chunk_t*) ((char*)(curr + 1) + size);
                split->size = orig_size - size - sizeof(malloc_chunk_t);
                split->total_size = split->size + sizeof(malloc_chunk_t);
                split->is_mmap = 0;
                split->next = curr->next;
                if (prev) {
                    prev->next = split;
                } else {
                    __libc_free_list = split;
                }
            } else {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    __libc_free_list = curr->next;
                }
            }
            curr->is_mmap = 0;
            curr->next = 0;
            return (void*)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }

    unsigned int req = sizeof(malloc_chunk_t) + size;
    unsigned int chunk_alloc = (req < 4096) ? 4096 : ((req + 4095) & ~4095u);
    void* p = sbrk(chunk_alloc);
    if (p == (void*)-1) return 0;

    malloc_chunk_t* chunk = (malloc_chunk_t*) p;
    chunk->size = size;
    chunk->total_size = chunk_alloc;
    chunk->is_mmap = 0;
    chunk->next = 0;

    if (chunk_alloc >= req + sizeof(malloc_chunk_t) + 16) {
        malloc_chunk_t* rem = (malloc_chunk_t*) ((char*)(chunk + 1) + size);
        rem->size = chunk_alloc - req - sizeof(malloc_chunk_t);
        rem->total_size = chunk_alloc - req;
        rem->is_mmap = 0;
        rem->next = __libc_free_list;
        __libc_free_list = rem;
    }

    return (void*)(chunk + 1);
}

static inline void free(void* ptr) {
    if (!ptr) return;
    malloc_chunk_t* chunk = ((malloc_chunk_t*) ptr) - 1;
    if (chunk->is_mmap) {
        munmap((void*)chunk, chunk->total_size);
    } else {
        chunk->next = __libc_free_list;
        __libc_free_list = chunk;
    }
}

static inline void* realloc(void* ptr, unsigned int new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return 0;
    }
    malloc_chunk_t* chunk = ((malloc_chunk_t*) ptr) - 1;
    if (chunk->size >= new_size) {
        return ptr;
    }
    void* new_ptr = malloc(new_size);
    if (!new_ptr) return 0;

    unsigned char* src = (unsigned char*) ptr;
    unsigned char* dst = (unsigned char*) new_ptr;
    unsigned int copy_len = chunk->size;
    for (unsigned int i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }
    free(ptr);
    return new_ptr;
}

static inline void* calloc(unsigned int nmemb, unsigned int size) {
    unsigned int total = nmemb * size;
    void* ptr = malloc(total);
    if (ptr) {
        unsigned char* p = (unsigned char*) ptr;
        for (unsigned int i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

/* Termios and ioctl support */
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;      /* input modes */
    tcflag_t c_oflag;      /* output modes */
    tcflag_t c_cflag;      /* control modes */
    tcflag_t c_lflag;      /* local modes */
    cc_t     c_line;       /* line discipline */
    cc_t     c_cc[NCCS];   /* control characters */
    speed_t  c_ispeed;     /* input baud rate */
    speed_t  c_ospeed;     /* output baud rate */
};

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004

/* c_cflag */
#define CS8     0000060
#define CREAD   0000200

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

/* c_cc indices */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSUSP   10
#define VWERASE 14

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

static inline int ioctl(int fd, unsigned long req, void* argp) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(54), "b"(fd), "c"(req), "d"(argp));
    return ret;
}

static inline int tcgetattr(int fd, struct termios* termios_p) {
    return ioctl(fd, TCGETS, termios_p);
}

static inline int tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    unsigned long req = TCSETS;
    if (optional_actions == TCSADRAIN) req = TCSETSW;
    else if (optional_actions == TCSAFLUSH) req = TCSETSF;
    return ioctl(fd, req, (void*) termios_p);
}

static inline int isatty(int fd) {
    struct winsize ws;
    return ioctl(fd, TIOCGWINSZ, &ws) == 0 ? 1 : 0;
}

/* Signal definitions and types */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGSTKFLT  16
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGTTIN    21
#define SIGTTOU    22
#define NSIG       32

typedef unsigned int sigset_t;

#define sigmask(sig) (1U << ((sig) - 1))

#define SIG_DFL ((void (*)(int)) 0)
#define SIG_IGN ((void (*)(int)) 1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_RESTORER  0x04000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

static inline int sigemptyset(sigset_t* set) {
    if (!set) return -1;
    *set = 0;
    return 0;
}

static inline int sigfillset(sigset_t* set) {
    if (!set) return -1;
    *set = 0xFFFFFFFFU;
    return 0;
}

static inline int sigaddset(sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) return -1;
    *set |= (1U << (signum - 1));
    return 0;
}

static inline int sigdelset(sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) return -1;
    *set &= ~(1U << (signum - 1));
    return 0;
}

static inline int sigismember(const sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= NSIG) return -1;
    return (*set & (1U << (signum - 1))) ? 1 : 0;
}

static inline int kill(int pid, int sig) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(37), "b"(pid), "c"(sig));
    return ret;
}

extern void __sigreturn_restorer(void);

static inline int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(67), "b"(sig), "c"(act), "d"(oldact));
    return ret;
}

static inline void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction act, oldact;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESTORER;
    act.sa_restorer = __sigreturn_restorer;
    if (sigaction(sig, &act, &oldact) < 0) {
        return (void (*)(int)) -1;
    }
    return oldact.sa_handler;
}

static inline int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(68), "b"(how), "c"(set), "d"(oldset));
    return ret;
}

static inline int pause(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(70));
    return ret;
}

static inline unsigned int alarm(unsigned int seconds) {
    unsigned int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(71), "b"(seconds));
    return ret;
}

static inline int setpgid(int pid, int pgid) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(57), "b"(pid), "c"(pgid));
    return ret;
}

static inline int getpgid(int pid) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(58), "b"(pid));
    return ret;
}

static inline int getpgrp(void) {
    return getpgid(0);
}

static inline int getppid(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(64));
    return ret;
}

static inline int getsid(int pid) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(65), "b"(pid));
    return ret;
}

static inline int setsid(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(66));
    return ret;
}

static inline int tcsetpgrp(int fd, int pgrp) {
    return ioctl(fd, TIOCSPGRP, &pgrp);
}

static inline int tcgetpgrp(int fd) {
    int pgrp = 0;
    if (ioctl(fd, TIOCGPGRP, &pgrp) < 0) return -1;
    return pgrp;
}

#endif


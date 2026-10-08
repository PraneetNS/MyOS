#ifndef LIBMYOS_SYSCALL_H
#define LIBMYOS_SYSCALL_H

#include <stdint.h>
#include <errno.h>

#define SYS_EXIT         0
#define SYS_WRITE        1
#define SYS_GETPID       2
#define SYS_OPEN         3
#define SYS_READ         4
#define SYS_CLOSE        5
#define SYS_FORK         7
#define SYS_PIPE         8
#define SYS_EXEC         10
#define SYS_WAITPID      11
#define SYS_SBRK         12
#define SYS_SYNC         13
#define SYS_LSEEK        14
#define SYS_STAT         15
#define SYS_FSTAT        16
#define SYS_GETDENTS     17
#define SYS_MKDIR        18
#define SYS_RMDIR        19
#define SYS_UNLINK       20
#define SYS_RENAME       21
#define SYS_CHDIR        22
#define SYS_GETCWD       23
#define SYS_DUP          24
#define SYS_DUP2         25
#define SYS_FCNTL        26
#define SYS_SLEEP        27
#define SYS_TICKS        28
#define SYS_FREE_FRAMES  29
#define SYS_MMAP         30
#define SYS_MUNMAP       31
#define SYS_MPROTECT     32
#define SYS_KILL         37
#define SYS_IOCTL        54
#define SYS_TCGETATTR    55
#define SYS_TCSETATTR    56
#define SYS_SETPGID      57
#define SYS_GETPGID      58
#define SYS_GETPPID      64
#define SYS_GETSID       65
#define SYS_SETSID       66
#define SYS_SIGACTION    67
#define SYS_SIGPROCMASK  68
#define SYS_SIGRETURN    119
#define SYS_PAUSE        70
#define SYS_ALARM        71
#define SYS_TIME         72
#define SYS_GETTIMEOFDAY 73
#define SYS_CLOCK_GETTIME 74
#define SYS_NANOSLEEP    75
#define SYS_NICE         77

static inline int __syscall0(int num) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int __syscall1(int num, long a1) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline int __syscall2(int num, long a1, long a2) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline int __syscall3(int num, long a1, long a2, long a3) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

static inline int __syscall4(int num, long a1, long a2, long a3, long a4) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

static inline int __syscall5(int num, long a1, long a2, long a3, long a4, long a5) {
    int ret;
    __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5) : "memory");
    return ret;
}

static inline int __syscall6(int num, long a1, long a2, long a3, long a4, long a5, long a6) {
    int ret;
    /* Use stack or ebp for 6th parameter */
    __asm__ __volatile__ (
        "pushl %7\n\t"
        "pushl %%ebp\n\t"
        "movl 4(%%esp), %%ebp\n\t"
        "int $0x80\n\t"
        "popl %%ebp\n\t"
        "addl $4, %%esp"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), "m"(a6)
        : "memory"
    );
    return ret;
}

static inline int __check_syscall_err(int ret) {
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

#endif

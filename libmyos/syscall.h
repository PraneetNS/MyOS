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
#define SYS_REBOOT       88
#define SYS_TRUNCATE     92
#define SYS_FTRUNCATE    93
#define SYS_SOCKET       102
#define SYS_BIND         103
#define SYS_LISTEN       104
#define SYS_ACCEPT       105
#define SYS_CONNECT      106
#define SYS_SHUTDOWN     107
#define SYS_GETSOCKNAME  108
#define SYS_GETPEERNAME  109
#define SYS_SOCKETPAIR   110
#define SYS_SEND         111
#define SYS_RECV         112
#define SYS_SETSOCKOPT   113
#define SYS_GETSOCKOPT   114
#define SYS_SENDTO       115
#define SYS_RECVFROM     116
#define SYS_MSYNC        144
#define SYS_POLL         168
#define SYS_AGENT_CREATE  170
#define SYS_AGENT_ATTACH  171
#define SYS_AGENT_KILL    172
#define SYS_AGENT_SUSPEND 173
#define SYS_AGENT_RESUME  174
#define SYS_AGENT_INFO    175
#define SYS_SOCK_PEERINFO        176
#define SYS_BUS_DAEMON_REGISTER  177
#define SYS_AUDIT_APPEND         178
#define SYS_AGENT_CHARGE_TOKENS  179
#define SYS_AGENT_TOKENS_LEFT    180
#define SYS_MODEL_DAEMON_REGISTER 181
#define SYS_AGENT_KILLSWITCH_STATE 182
#define SYS_AGENT_CHARGE_NET     183
#define SYS_AGENT_NET_LEFT       184
#define SYS_NET_DAEMON_REGISTER  185

/* On x86-64:
   Fast syscall convention uses `syscall`:
     %rax = syscall number
     %rdi = arg1
     %rsi = arg2
     %rdx = arg3
     %r10 = arg4 (standard x86-64 syscall clobbers rcx with rip, r11 with rflags)
     %r8  = arg5
     %r9  = arg6
     Return value in %rax
*/

static inline long __syscall0(long num) {
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall1(long num, long a1) {
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall5(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long __syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int __check_syscall_err(long ret) {
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

#endif

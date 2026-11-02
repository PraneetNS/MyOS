#ifndef LIBMYOS_SYSCALL_H
#define LIBMYOS_SYSCALL_H

#include <stdint.h>
#include <errno.h>
#include "myos_syscalls.h"

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

static inline long syscall(long num, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, num);
    long a1 = __builtin_va_arg(ap, long);
    long a2 = __builtin_va_arg(ap, long);
    long a3 = __builtin_va_arg(ap, long);
    long a4 = __builtin_va_arg(ap, long);
    long a5 = __builtin_va_arg(ap, long);
    long a6 = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
    return __syscall6(num, a1, a2, a3, a4, a5, a6);
}

static inline int __translate_linux_errno(int kerr) {
    switch (kerr) {
        case 35:  return EDEADLK;
        case 36:  return ENAMETOOLONG;
        case 37:  return ENOLCK;
        case 38:  return ENOSYS;
        case 39:  return ENOTEMPTY;
        case 40:  return ELOOP;
        case 42:  return ENOMSG;
        case 75:  return EOVERFLOW;
        case 84:  return EILSEQ;
        case 88:  return ENOTSOCK;
        case 89:  return EDESTADDRREQ;
        case 90:  return EMSGSIZE;
        case 91:  return EPROTOTYPE;
        case 92:  return ENOPROTOOPT;
        case 93:  return EPROTONOSUPPORT;
        case 95:  return EOPNOTSUPP;
        case 96:  return EPFNOSUPPORT;
        case 97:  return EAFNOSUPPORT;
        case 98:  return EADDRINUSE;
        case 99:  return EADDRNOTAVAIL;
        case 100: return ENETDOWN;
        case 101: return ENETUNREACH;
        case 102: return ENETRESET;
        case 103: return ECONNABORTED;
        case 104: return ECONNRESET;
        case 105: return ENOBUFS;
        case 106: return EISCONN;
        case 107: return ENOTCONN;
        case 109: return ETOOMANYREFS;
        case 110: return ETIMEDOUT;
        case 111: return ECONNREFUSED;
        case 112: return EHOSTDOWN;
        case 113: return EHOSTUNREACH;
        case 114: return EALREADY;
        case 115: return EINPROGRESS;
        case 116: return ESTALE;
        case 122: return EDQUOT;
        case 125: return ECANCELED;
        default:  return kerr;
    }
}

static inline int __check_syscall_err(long ret) {
    if (ret < 0) {
        errno = __translate_linux_errno((int)(-ret));
        return -1;
    }
    return (int)ret;
}

#endif

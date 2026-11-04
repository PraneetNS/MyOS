#ifndef MYOS_SYSCALLS_H
#define MYOS_SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

/* Linux x86-64 Standard Syscall Numbers */
#define __NR_read                  0
#define __NR_write                 1
#define __NR_open                  2
#define __NR_close                 3
#define __NR_stat                  4
#define __NR_fstat                 5
#define __NR_lstat                 6
#define __NR_poll                  7
#define __NR_lseek                 8
#define __NR_mmap                  9
#define __NR_mprotect             10
#define __NR_munmap               11
#define __NR_brk                  12
#define __NR_rt_sigaction         13
#define __NR_rt_sigprocmask       14
#define __NR_rt_sigreturn         15
#define __NR_ioctl                16
#define __NR_pread64              17
#define __NR_pwrite64             18
#define __NR_readv                19
#define __NR_writev               20
#define __NR_access               21
#define __NR_pipe                 22
#define __NR_select               23
#define __NR_sched_yield          24
#define __NR_msync                26
#define __NR_madvise              28
#define __NR_dup                  32
#define __NR_dup2                 33
#define __NR_pause                34
#define __NR_nanosleep            35
#define __NR_alarm                37
#define __NR_getpid               39
#define __NR_sendfile             40
#define __NR_socket               41
#define __NR_connect              42
#define __NR_accept               43
#define __NR_sendto               44
#define __NR_recvfrom             45
#define __NR_shutdown             48
#define __NR_bind                 49
#define __NR_listen               50
#define __NR_getsockname          51
#define __NR_getpeername          52
#define __NR_socketpair           53
#define __NR_setsockopt           54
#define __NR_getsockopt           55
#define __NR_clone                56
#define __NR_fork                 57
#define __NR_vfork                58
#define __NR_execve               59
#define __NR_exit                 60
#define __NR_wait4                61
#define __NR_kill                 62
#define __NR_uname                63
#define __NR_fcntl                72
#define __NR_fsync                74
#define __NR_fdatasync            75
#define __NR_truncate             76
#define __NR_ftruncate            77
#define __NR_getdents             78
#define __NR_getcwd               79
#define __NR_chdir                80
#define __NR_fchdir               81
#define __NR_rename               82
#define __NR_mkdir                83
#define __NR_rmdir                84
#define __NR_creat                85
#define __NR_unlink               87
#define __NR_symlink              88
#define __NR_readlink             89
#define __NR_chmod                90
#define __NR_fchmod               91
#define __NR_chown                92
#define __NR_fchown               93
#define __NR_lchown               94
#define __NR_umask                95
#define __NR_gettimeofday         96
#define __NR_getrlimit            97
#define __NR_getrusage            98
#define __NR_sysinfo              99
#define __NR_getuid              102
#define __NR_getgid              104
#define __NR_setuid              105
#define __NR_setgid              106
#define __NR_geteuid             107
#define __NR_getegid             108
#define __NR_setpgid             109
#define __NR_getppid             110
#define __NR_getpgrp             111
#define __NR_setsid              112
#define __NR_setreuid            113
#define __NR_setregid            114
#define __NR_getgroups           115
#define __NR_setgroups           116
#define __NR_setresuid           117
#define __NR_getresuid           118
#define __NR_setresgid           119
#define __NR_getresgid           120
#define __NR_getpgid             121
#define __NR_getsid              124
#define __NR_rt_sigpending       127
#define __NR_rt_sigtimedwait     128
#define __NR_rt_sigqueueinfo     129
#define __NR_rt_sigsuspend       130
#define __NR_sigaltstack         131
#define __NR_statfs              137
#define __NR_fstatfs             138
#define __NR_setpriority         141
#define __NR_prctl               157
#define __NR_arch_prctl          158
#define __NR_setrlimit           160
#define __NR_reboot              169
#define __NR_gettid              186
#define __NR_tkill               200
#define __NR_time                201
#define __NR_futex               202
#define __NR_sched_setaffinity   203
#define __NR_sched_getaffinity   204
#define __NR_getdents64          217
#define __NR_set_tid_address     218
#define __NR_clock_gettime       228
#define __NR_clock_getres        229
#define __NR_clock_nanosleep     230
#define __NR_exit_group          231
#define __NR_tgkill              234
#define __NR_openat              257
#define __NR_mkdirat             258
#define __NR_fchownat            260
#define __NR_newfstatat          262
#define __NR_unlinkat            263
#define __NR_renameat            264
#define __NR_symlinkat           266
#define __NR_readlinkat          267
#define __NR_fchmodat            268
#define __NR_faccessat           269
#define __NR_pselect6            270
#define __NR_ppoll               271
#define __NR_set_robust_list     273
#define __NR_get_robust_list     274
#define __NR_utimensat           280
#define __NR_accept4             288
#define __NR_dup3                292
#define __NR_pipe2               293
#define __NR_prlimit64           302
#define __NR_getcpu              309
#define __NR_renameat2           316
#define __NR_getrandom           318
#define __NR_membarrier          324
#define __NR_statx               332
#define __NR_rseq                334
#define __NR_faccessat2          439

/* arch_prctl subcommands */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* MyOS Custom Syscall Range (1000+) */
#define SYS_TICKS                  1000
#define SYS_FREE_FRAMES            1001
#define SYS_AGENT_CREATE           1002
#define SYS_AGENT_ATTACH           1003
#define SYS_AGENT_KILL             1004
#define SYS_AGENT_SUSPEND          1005
#define SYS_AGENT_RESUME           1006
#define SYS_AGENT_INFO             1007
#define SYS_SOCK_PEERINFO          1008
#define SYS_BUS_DAEMON_REGISTER    1009
#define SYS_AUDIT_APPEND           1010
#define SYS_AGENT_CHARGE_TOKENS    1011
#define SYS_AGENT_TOKENS_LEFT      1012
#define SYS_MODEL_DAEMON_REGISTER  1013
#define SYS_AGENT_KILLSWITCH_STATE 1014
#define SYS_AGENT_CHARGE_NET       1015
#define SYS_AGENT_NET_LEFT         1016
#define SYS_NET_DAEMON_REGISTER    1017
#define SYS_TEST_HARDENING         1018

/* Native POSIX aliases mapped to Linux syscall numbers */
#define SYS_READ         __NR_read
#define SYS_WRITE        __NR_write
#define SYS_OPEN         __NR_open
#define SYS_CLOSE        __NR_close
#define SYS_STAT         __NR_stat
#define SYS_FSTAT        __NR_fstat
#define SYS_LSTAT        __NR_lstat
#define SYS_POLL         __NR_poll
#define SYS_LSEEK        __NR_lseek
#define SYS_MMAP         __NR_mmap
#define SYS_MPROTECT     __NR_mprotect
#define SYS_MUNMAP       __NR_munmap
#define SYS_BRK          __NR_brk
#define SYS_SBRK         __NR_brk
#define SYS_RT_SIGACTION __NR_rt_sigaction
#define SYS_SIGACTION    __NR_rt_sigaction
#define SYS_RT_SIGPROCMASK __NR_rt_sigprocmask
#define SYS_SIGPROCMASK  __NR_rt_sigprocmask
#define SYS_RT_SIGRETURN __NR_rt_sigreturn
#define SYS_SIGRETURN    __NR_rt_sigreturn
#define SYS_IOCTL        __NR_ioctl
#define SYS_PREAD64      __NR_pread64
#define SYS_PWRITE64     __NR_pwrite64
#define SYS_READV        __NR_readv
#define SYS_WRITEV       __NR_writev
#define SYS_ACCESS       __NR_access
#define SYS_PIPE         __NR_pipe
#define SYS_SELECT       __NR_select
#define SYS_DUP          __NR_dup
#define SYS_DUP2         __NR_dup2
#define SYS_PAUSE        __NR_pause
#define SYS_NANOSLEEP    __NR_nanosleep
#define SYS_ALARM        __NR_alarm
#define SYS_GETPID       __NR_getpid
#define SYS_SOCKET       __NR_socket
#define SYS_CONNECT      __NR_connect
#define SYS_ACCEPT       __NR_accept
#define SYS_SENDTO       __NR_sendto
#define SYS_RECVFROM     __NR_recvfrom
#define SYS_SHUTDOWN     __NR_shutdown
#define SYS_BIND         __NR_bind
#define SYS_LISTEN       __NR_listen
#define SYS_GETSOCKNAME  __NR_getsockname
#define SYS_GETPEERNAME  __NR_getpeername
#define SYS_SOCKETPAIR   __NR_socketpair
#define SYS_SETSOCKOPT   __NR_setsockopt
#define SYS_GETSOCKOPT   __NR_getsockopt
#define SYS_CLONE        __NR_clone
#define SYS_FORK         __NR_fork
#define SYS_EXEC         __NR_execve
#define SYS_EXECVE       __NR_execve
#define SYS_EXIT         __NR_exit
#define SYS_WAITPID      __NR_wait4
#define SYS_KILL         __NR_kill
#define SYS_UNAME        __NR_uname
#define SYS_FCNTL        __NR_fcntl
#define SYS_FSYNC        __NR_fsync
#define SYS_SYNC         __NR_fsync
#define SYS_FDATASYNC    __NR_fdatasync
#define SYS_TRUNCATE     __NR_truncate
#define SYS_FTRUNCATE    __NR_ftruncate
#define SYS_GETDENTS     __NR_getdents64
#define SYS_GETCWD       __NR_getcwd
#define SYS_CHDIR        __NR_chdir
#define SYS_FCHDIR       __NR_fchdir
#define SYS_RENAME       __NR_rename
#define SYS_MKDIR        __NR_mkdir
#define SYS_RMDIR        __NR_rmdir
#define SYS_UNLINK       __NR_unlink
#define SYS_SYMLINK      __NR_symlink
#define SYS_READLINK     __NR_readlink
#define SYS_CHMOD        __NR_chmod
#define SYS_FCHMOD       __NR_fchmod
#define SYS_CHOWN        __NR_chown
#define SYS_UMASK        __NR_umask
#define SYS_GETTIMEOFDAY __NR_gettimeofday
#define SYS_GETRLIMIT    __NR_getrlimit
#define SYS_SYSINFO      __NR_sysinfo
#define SYS_GETUID       __NR_getuid
#define SYS_GETGID       __NR_getgid
#define SYS_SETUID       __NR_setuid
#define SYS_SETGID       __NR_setgid
#define SYS_GETEUID      __NR_geteuid
#define SYS_GETEGID      __NR_getegid
#define SYS_SETPGID      __NR_setpgid
#define SYS_GETPPID      __NR_getppid
#define SYS_GETPGRP      __NR_getpgrp
#define SYS_SETSID       __NR_setsid
#define SYS_GETPGID      __NR_getpgid
#define SYS_GETSID       __NR_getsid
#define SYS_CLOCK_GETTIME __NR_clock_gettime
#define SYS_EXIT_GROUP   __NR_exit_group
#define SYS_OPENAT       __NR_openat
#define SYS_NEWFSTATAT   __NR_newfstatat
#define SYS_UNLINKAT     __NR_unlinkat
#define SYS_RENAMEAT     __NR_renameat
#define SYS_RENAMEAT2    __NR_renameat2
#define SYS_READLINKAT   __NR_readlinkat
#define SYS_SYMLINKAT    __NR_symlinkat
#define SYS_FCHMODAT     __NR_fchmodat
#define SYS_FACCESSAT    __NR_faccessat
#define SYS_UTIMENSAT    __NR_utimensat
#define SYS_DUP3         __NR_dup3
#define SYS_PIPE2        __NR_pipe2
#define SYS_PRLIMIT64    __NR_prlimit64
#define SYS_GETRANDOM    __NR_getrandom
#define SYS_SLEEP        __NR_nanosleep
#define SYS_SEND         __NR_sendto
#define SYS_RECV         __NR_recvfrom
#define SYS_MSYNC        __NR_msync
#define SYS_NICE         __NR_setpriority
#define SYS_REBOOT       __NR_reboot
#define SYS_TIME         __NR_time
#define SYS_TCGETATTR    1020
#define SYS_TCSETATTR    1021

/* Linux x86-64 standard data structures */
#if !defined(_SYS__TIMESPEC_H_) && !defined(_STRUCT_TIMESPEC) && !defined(_TIMESPEC_DECLARED) && !defined(__timespec_defined)
#define _SYS__TIMESPEC_H_
#define _STRUCT_TIMESPEC
#define _TIMESPEC_DECLARED
#define __timespec_defined
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
#endif

#ifndef _STRUCT_IOVEC
#define _STRUCT_IOVEC
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#endif

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

struct linux_stat64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    int64_t  __pad_unused[3];
};

struct linux_pollfd {
    int fd;
    short events;
    short revents;
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char _f[20-2*sizeof(int64_t)-sizeof(int32_t)];
};

struct linux_rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

#endif /* MYOS_SYSCALLS_H */
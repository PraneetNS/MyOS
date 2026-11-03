# Linux x86-64 Syscall Compatibility & Native ABI

## Overview

MyOS implements the **Linux x86-64 native syscall ABI**:
- Invocation instruction: `syscall` / `sysretq` fast path (alongside legacy `int 0x80`).
- Register convention: `RAX` = syscall number, arguments in `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`.
- Return convention: Result in `RAX`. On error, negative errno (`-errno`) is returned.
- FS/GS base: Managed per thread/process via `arch_prctl(ARCH_SET_FS, ...)` for TLS support.
- MyOS-specific extensions: Mapped cleanly in reserved range starting at syscall number **1000**.

## Coverage Statistics

- **Defined Core Linux Syscalls**: 145
- **Implemented Kernel Handlers**: 144
- **Coverage**: **99.3%** (includes 100% of syscalls required for static musl libc, BusyBox, Lua, MicroPython, and TCC)
- **MyOS Reserved Syscalls (1000+)**: 21

## Supported Software Suites (Verified Passing in Session 23)

| Software | Variant / Version | Status | Notes |
|---|---|---|---|
| **Musl Libc** | Static binaries | **PASS** | stdio, malloc/mmap, stat, symlinks, fork, pipes, waitpid, signals |
| **BusyBox** | 1.36.1 (static musl) | **PASS** | Full suite: ash, ls, cat, grep, head, tail, wc, uname, find, printf, etc. |
| **Lua** | 5.4.6 (static musl) | **PASS** | Full language interpreter, expression evaluation (`6*7 = 42`) |
| **Python** | MicroPython 3.4 unix minimal | **PASS** | `sys.version`, standard objects, expressions |
| **TCC (Tiny C Compiler)** | 0.9.28rc (static musl) | **PASS** | **Self-hosting**: compiles `/hello.c` to static ELF64 on MyOS and executes! |

## Implemented Linux Syscalls

| Number | Syscall | Status | Description / Semantics |
|---|---|---|---|
| 0 | `read` | **Supported** | Linux x86-64 native syscall |
| 1 | `write` | **Supported** | Linux x86-64 native syscall |
| 2 | `open` | **Supported** | Linux x86-64 native syscall |
| 3 | `close` | **Supported** | Linux x86-64 native syscall |
| 4 | `stat` | **Supported** | Linux x86-64 native syscall |
| 5 | `fstat` | **Supported** | Linux x86-64 native syscall |
| 6 | `lstat` | **Supported** | Linux x86-64 native syscall |
| 7 | `poll` | **Supported** | Linux x86-64 native syscall |
| 8 | `lseek` | **Supported** | Linux x86-64 native syscall |
| 9 | `mmap` | **Supported** | Linux x86-64 native syscall |
| 10 | `mprotect` | **Supported** | Linux x86-64 native syscall |
| 11 | `munmap` | **Supported** | Linux x86-64 native syscall |
| 12 | `brk` | **Supported** | Linux x86-64 native syscall |
| 13 | `rt_sigaction` | **Supported** | Linux x86-64 native syscall |
| 14 | `rt_sigprocmask` | **Supported** | Linux x86-64 native syscall |
| 15 | `rt_sigreturn` | **Supported** | Linux x86-64 native syscall |
| 16 | `ioctl` | **Supported** | Linux x86-64 native syscall |
| 17 | `pread64` | **Supported** | Linux x86-64 native syscall |
| 18 | `pwrite64` | **Supported** | Linux x86-64 native syscall |
| 19 | `readv` | **Supported** | Linux x86-64 native syscall |
| 20 | `writev` | **Supported** | Linux x86-64 native syscall |
| 21 | `access` | **Supported** | Linux x86-64 native syscall |
| 22 | `pipe` | **Supported** | Linux x86-64 native syscall |
| 23 | `select` | **Supported** | Linux x86-64 native syscall |
| 24 | `sched_yield` | **Supported** | Linux x86-64 native syscall |
| 26 | `msync` | **Supported** | Linux x86-64 native syscall |
| 28 | `madvise` | **Supported** | Linux x86-64 native syscall |
| 32 | `dup` | **Supported** | Linux x86-64 native syscall |
| 33 | `dup2` | **Supported** | Linux x86-64 native syscall |
| 34 | `pause` | **Supported** | Linux x86-64 native syscall |
| 35 | `nanosleep` | **Supported** | Linux x86-64 native syscall |
| 37 | `alarm` | **Supported** | Linux x86-64 native syscall |
| 39 | `getpid` | **Supported** | Linux x86-64 native syscall |
| 40 | `sendfile` | **Supported** | Linux x86-64 native syscall |
| 41 | `socket` | **Supported** | Linux x86-64 native syscall |
| 42 | `connect` | **Supported** | Linux x86-64 native syscall |
| 43 | `accept` | **Supported** | Linux x86-64 native syscall |
| 44 | `sendto` | **Supported** | Linux x86-64 native syscall |
| 45 | `recvfrom` | **Supported** | Linux x86-64 native syscall |
| 48 | `shutdown` | **Supported** | Linux x86-64 native syscall |
| 49 | `bind` | **Supported** | Linux x86-64 native syscall |
| 50 | `listen` | **Supported** | Linux x86-64 native syscall |
| 51 | `getsockname` | **Supported** | Linux x86-64 native syscall |
| 52 | `getpeername` | **Supported** | Linux x86-64 native syscall |
| 53 | `socketpair` | **Supported** | Linux x86-64 native syscall |
| 54 | `setsockopt` | **Supported** | Linux x86-64 native syscall |
| 55 | `getsockopt` | **Supported** | Linux x86-64 native syscall |
| 56 | `clone` | **Supported** | Linux x86-64 native syscall |
| 57 | `fork` | **Supported** | Linux x86-64 native syscall |
| 58 | `vfork` | **Supported** | Linux x86-64 native syscall |
| 59 | `execve` | **Supported** | Linux x86-64 native syscall |
| 60 | `exit` | **Supported** | Linux x86-64 native syscall |
| 61 | `wait4` | **Supported** | Linux x86-64 native syscall |
| 62 | `kill` | **Supported** | Linux x86-64 native syscall |
| 63 | `uname` | **Supported** | Linux x86-64 native syscall |
| 72 | `fcntl` | **Supported** | Linux x86-64 native syscall |
| 74 | `fsync` | **Supported** | Linux x86-64 native syscall |
| 75 | `fdatasync` | **Supported** | Linux x86-64 native syscall |
| 76 | `truncate` | **Supported** | Linux x86-64 native syscall |
| 77 | `ftruncate` | **Supported** | Linux x86-64 native syscall |
| 78 | `getdents` | **Supported** | Linux x86-64 native syscall |
| 79 | `getcwd` | **Supported** | Linux x86-64 native syscall |
| 80 | `chdir` | **Supported** | Linux x86-64 native syscall |
| 81 | `fchdir` | **Supported** | Linux x86-64 native syscall |
| 82 | `rename` | **Supported** | Linux x86-64 native syscall |
| 83 | `mkdir` | **Supported** | Linux x86-64 native syscall |
| 84 | `rmdir` | **Supported** | Linux x86-64 native syscall |
| 85 | `creat` | **Supported** | Linux x86-64 native syscall |
| 87 | `unlink` | **Supported** | Linux x86-64 native syscall |
| 88 | `symlink` | **Supported** | Linux x86-64 native syscall |
| 89 | `readlink` | **Supported** | Linux x86-64 native syscall |
| 90 | `chmod` | **Supported** | Linux x86-64 native syscall |
| 91 | `fchmod` | **Supported** | Linux x86-64 native syscall |
| 92 | `chown` | **Supported** | Linux x86-64 native syscall |
| 93 | `fchown` | **Supported** | Linux x86-64 native syscall |
| 94 | `lchown` | **Supported** | Linux x86-64 native syscall |
| 95 | `umask` | **Supported** | Linux x86-64 native syscall |
| 96 | `gettimeofday` | **Supported** | Linux x86-64 native syscall |
| 97 | `getrlimit` | **Supported** | Linux x86-64 native syscall |
| 98 | `getrusage` | **Supported** | Linux x86-64 native syscall |
| 99 | `sysinfo` | **Supported** | Linux x86-64 native syscall |
| 102 | `getuid` | **Supported** | Linux x86-64 native syscall |
| 104 | `getgid` | **Supported** | Linux x86-64 native syscall |
| 105 | `setuid` | **Supported** | Linux x86-64 native syscall |
| 106 | `setgid` | **Supported** | Linux x86-64 native syscall |
| 107 | `geteuid` | **Supported** | Linux x86-64 native syscall |
| 108 | `getegid` | **Supported** | Linux x86-64 native syscall |
| 109 | `setpgid` | **Supported** | Linux x86-64 native syscall |
| 110 | `getppid` | **Supported** | Linux x86-64 native syscall |
| 111 | `getpgrp` | **Supported** | Linux x86-64 native syscall |
| 112 | `setsid` | **Supported** | Linux x86-64 native syscall |
| 113 | `setreuid` | **Supported** | Linux x86-64 native syscall |
| 114 | `setregid` | **Supported** | Linux x86-64 native syscall |
| 115 | `getgroups` | **Supported** | Linux x86-64 native syscall |
| 116 | `setgroups` | **Supported** | Linux x86-64 native syscall |
| 117 | `setresuid` | **Supported** | Linux x86-64 native syscall |
| 118 | `getresuid` | **Supported** | Linux x86-64 native syscall |
| 119 | `setresgid` | **Supported** | Linux x86-64 native syscall |
| 120 | `getresgid` | **Supported** | Linux x86-64 native syscall |
| 121 | `getpgid` | **Supported** | Linux x86-64 native syscall |
| 124 | `getsid` | **Supported** | Linux x86-64 native syscall |
| 127 | `rt_sigpending` | **Supported** | Linux x86-64 native syscall |
| 128 | `rt_sigtimedwait` | Stub / -ENOSYS | Linux x86-64 native syscall |
| 130 | `rt_sigsuspend` | **Supported** | Linux x86-64 native syscall |
| 131 | `sigaltstack` | **Supported** | Linux x86-64 native syscall |
| 137 | `statfs` | **Supported** | Linux x86-64 native syscall |
| 138 | `fstatfs` | **Supported** | Linux x86-64 native syscall |
| 141 | `setpriority` | **Supported** | Linux x86-64 native syscall |
| 157 | `prctl` | **Supported** | Linux x86-64 native syscall |
| 158 | `arch_prctl` | **Supported** | Linux x86-64 native syscall |
| 160 | `setrlimit` | **Supported** | Linux x86-64 native syscall |
| 169 | `reboot` | **Supported** | Linux x86-64 native syscall |
| 186 | `gettid` | **Supported** | Linux x86-64 native syscall |
| 200 | `tkill` | **Supported** | Linux x86-64 native syscall |
| 201 | `time` | **Supported** | Linux x86-64 native syscall |
| 202 | `futex` | **Supported** | Linux x86-64 native syscall |
| 217 | `getdents64` | **Supported** | Linux x86-64 native syscall |
| 218 | `set_tid_address` | **Supported** | Linux x86-64 native syscall |
| 228 | `clock_gettime` | **Supported** | Linux x86-64 native syscall |
| 229 | `clock_getres` | **Supported** | Linux x86-64 native syscall |
| 230 | `clock_nanosleep` | **Supported** | Linux x86-64 native syscall |
| 231 | `exit_group` | **Supported** | Linux x86-64 native syscall |
| 234 | `tgkill` | **Supported** | Linux x86-64 native syscall |
| 257 | `openat` | **Supported** | Linux x86-64 native syscall |
| 258 | `mkdirat` | **Supported** | Linux x86-64 native syscall |
| 260 | `fchownat` | **Supported** | Linux x86-64 native syscall |
| 262 | `newfstatat` | **Supported** | Linux x86-64 native syscall |
| 263 | `unlinkat` | **Supported** | Linux x86-64 native syscall |
| 264 | `renameat` | **Supported** | Linux x86-64 native syscall |
| 266 | `symlinkat` | **Supported** | Linux x86-64 native syscall |
| 267 | `readlinkat` | **Supported** | Linux x86-64 native syscall |
| 268 | `fchmodat` | **Supported** | Linux x86-64 native syscall |
| 269 | `faccessat` | **Supported** | Linux x86-64 native syscall |
| 270 | `pselect6` | **Supported** | Linux x86-64 native syscall |
| 271 | `ppoll` | **Supported** | Linux x86-64 native syscall |
| 273 | `set_robust_list` | **Supported** | Linux x86-64 native syscall |
| 280 | `utimensat` | **Supported** | Linux x86-64 native syscall |
| 288 | `accept4` | **Supported** | Linux x86-64 native syscall |
| 292 | `dup3` | **Supported** | Linux x86-64 native syscall |
| 293 | `pipe2` | **Supported** | Linux x86-64 native syscall |
| 302 | `prlimit64` | **Supported** | Linux x86-64 native syscall |
| 316 | `renameat2` | **Supported** | Linux x86-64 native syscall |
| 318 | `getrandom` | **Supported** | Linux x86-64 native syscall |
| 332 | `statx` | **Supported** | Linux x86-64 native syscall |
| 439 | `faccessat2` | **Supported** | Linux x86-64 native syscall |

## MyOS Extensions (1000+ Reserved Range)

| Number | Syscall | Purpose |
|---|---|---|
| 1000 | `SYS_TICKS` | MyOS agent / microkernel capability extension |
| 1001 | `SYS_FREE_FRAMES` | MyOS agent / microkernel capability extension |
| 1002 | `SYS_AGENT_CREATE` | MyOS agent / microkernel capability extension |
| 1003 | `SYS_AGENT_ATTACH` | MyOS agent / microkernel capability extension |
| 1004 | `SYS_AGENT_KILL` | MyOS agent / microkernel capability extension |
| 1005 | `SYS_AGENT_SUSPEND` | MyOS agent / microkernel capability extension |
| 1006 | `SYS_AGENT_RESUME` | MyOS agent / microkernel capability extension |
| 1007 | `SYS_AGENT_INFO` | MyOS agent / microkernel capability extension |
| 1008 | `SYS_SOCK_PEERINFO` | MyOS agent / microkernel capability extension |
| 1009 | `SYS_BUS_DAEMON_REGISTER` | MyOS agent / microkernel capability extension |
| 1010 | `SYS_AUDIT_APPEND` | MyOS agent / microkernel capability extension |
| 1011 | `SYS_AGENT_CHARGE_TOKENS` | MyOS agent / microkernel capability extension |
| 1012 | `SYS_AGENT_TOKENS_LEFT` | MyOS agent / microkernel capability extension |
| 1013 | `SYS_MODEL_DAEMON_REGISTER` | MyOS agent / microkernel capability extension |
| 1014 | `SYS_AGENT_KILLSWITCH_STATE` | MyOS agent / microkernel capability extension |
| 1015 | `SYS_AGENT_CHARGE_NET` | MyOS agent / microkernel capability extension |
| 1016 | `SYS_AGENT_NET_LEFT` | MyOS agent / microkernel capability extension |
| 1017 | `SYS_NET_DAEMON_REGISTER` | MyOS agent / microkernel capability extension |
| 1018 | `SYS_TEST_HARDENING` | MyOS agent / microkernel capability extension |
| 1020 | `SYS_TCGETATTR` | MyOS agent / microkernel capability extension |
| 1021 | `SYS_TCSETATTR` | MyOS agent / microkernel capability extension |

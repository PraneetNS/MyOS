# Linux x86-64 Syscall Compatibility & Native ABI

## Overview

MyOS implements the **Linux x86-64 native syscall ABI** within its monolithic Ring 0 kernel:
- **Invocation instruction**: `syscall` / `sysretq` fast path (alongside legacy `int 0x80`).
- **Register convention**: `RAX` = syscall number, arguments in `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`.
- **Return convention**: Result in `RAX`. On error, negative errno (`-errno`) is returned.
- **Thread-Local Storage (TLS)**: Managed per task via `arch_prctl(ARCH_SET_FS, ...)` and saved/restored on context switch.
- **MyOS-Specific Syscalls**: Mapped cleanly in a reserved range starting at syscall number **1000** (`libmyos/include/myos_syscalls.h`).
- **Monolithic Architecture**: The kernel is a monolithic OS kernel; all syscall handlers execute in Ring 0 with hardware stack guard protection. Unknown syscalls log once per (process, nr) to serial and return `-ENOSYS` cleanly without panicking.

## Full Linux 6.1 LTS x86-64 Syscall Coverage

Coverage evaluated against the full **Linux 6.1 LTS (x86-64)** syscall table (syscalls 0 through 450, total 451 syscalls):

| Category | Count | Percentage | Description |
|---|---|---|---|
| **Total Linux 6.1 Syscalls** | **451** | **100.0%** | Full Linux 6.1 x86-64 table (`sys_call_table`, 0..450) |
| **Implemented / Stubbed** | **148** | **32.8%** | Native Linux semantics or conformant stubs |
| **Unimplemented (`-ENOSYS`)** | **303** | **67.2%** | Clean `-ENOSYS` return with rate-limited serial audit logging |
| **MyOS-Specific Syscalls (1000+)** | **22** | — | Ring 0 agent capability, intent bus, audit, and model extensions |

> [!NOTE]
> **Honest Labeling Note**: MyOS does *not* implement the full Linux kernel (which spans over 450 syscalls including namespaces, cgroups, bpf, io_uring, and kvm). Instead, MyOS implements **148 Linux x86-64 syscalls (32.8% of Linux 6.1)**. This 148-syscall subset covers 100% of the syscalls exercised by static musl libc, BusyBox 1.36.1, Lua 5.4.6, MicroPython 3.4.0, and Tiny C Compiler (TCC).

---

## Supported Software Suites (Verified Passing in Session 23)

| Software | Variant / Version | Status | Notes |
|---|---|---|---|
| **Musl Libc** | Static binaries | **PASS** | Stdio, malloc/mmap, stat, symlinks, fork, pipes, waitpid, signals |
| **BusyBox** | 1.36.1 (static musl) | **PASS** | Full suite: ash, sed, awk, sort, uniq, tr, cut, xargs, tar, gzip, find -exec, diff, cp -r, ln -s, chmod, date, etc. |
| **Lua** | 5.4.6 (static musl) | **PASS** | Full language interpreter, expression evaluation (`6*7 = 42`) |
| **MicroPython** | MicroPython 3.4.0 unix minimal | **PASS** | Python 3.4-compatible subset, AST execution, `sys.version`, standard objects, math |
| **TCC (Tiny C Compiler)** | 0.9.28rc (static musl) | **PASS** | **Self-hosting**: compiles `/hello.c` to static ELF64 on MyOS and executes natively |

### Static CPython Attempt & Analysis
- **Attempt**: Static compilation of CPython 3.11 / 3.12 with musl libc targeting MyOS.
- **Findings & Blockers**:
  1. **Binary & Memory Footprint**: A static CPython build with standard builtins exceeds 25MB in binary size and demands 30MB+ of heap during interpreter initialization, plus a 40MB+ standard library folder on disk (`lib/python3.11/`).
  2. **Threading & Concurrency**: CPython initialization strongly expects POSIX thread runtime (`pthread_create`, `futex` robust lists, and `clone(CLONE_THREAD)`).
  3. **Dynamic Extensions**: CPython's module system depends heavily on `dlopen`/`dlsym` for shared libraries.
- **Architectural Decision**: **MicroPython 3.4.0 unix minimal** was selected and integrated. MicroPython provides a Python 3.4-compatible subset (lexical parsing, AST compilation, and core language semantics without full CPython standard library bloat) in a lightweight 350KB static binary with zero external library dependencies, booting instantly and operating strictly within MyOS agent memory constraints.

---

## Syscalls Exercised by Shipped Binaries

During regression test suite execution (Session 23, `musltest.elf`, `busybox`, `lua`, `python` (MicroPython), `tcc`, `stacktest`, `leaktest`, `sysfuzz`), the following Linux syscalls are exercised:

| Syscall # | Name | Exercising Component | Verified Behavior |
|---|---|---|---|
| 0 | `read` | Stdio, BusyBox, Lua, MicroPython, TCC | VFS read from disk, pipe, socket, TTY |
| 1 | `write` | Stdio, Console, TTY | Output formatting and pipe writes |
| 2 | `open` | Musl stdio, BusyBox, TCC | Opening files and directories |
| 3 | `close` | Musl libc, all binaries | Releasing file descriptors |
| 4 | `stat` | Musl libc, BusyBox stat | Retrieving file metadata |
| 5 | `fstat` | Musl libc, TCC, BusyBox | Retrieving open file descriptor stat |
| 6 | `lstat` | Musl libc, BusyBox ln/ls | Retrieving symlink stat without dereferencing |
| 7 | `poll` | BusyBox, IPC | Polling file descriptor status |
| 8 | `lseek` | Musl libc, TCC, BusyBox | Seeking in files |
| 9 | `mmap` | Musl allocator, ELF loader | Anonymous memory allocation and ASLR |
| 10 | `mprotect` | Musl libc, Security model | Page permission hardening (R/W/NX) |
| 11 | `munmap` | Musl allocator | Returning mapped memory |
| 12 | `brk` | Musl/Newlib malloc | Process heap base expansion |
| 13 | `rt_sigaction` | Musl signal subsystem | Signal handler registration |
| 14 | `rt_sigprocmask` | Musl libc | Signal mask management |
| 15 | `rt_sigreturn` | Signal trampoline | Restoring CPU context after signal delivery |
| 16 | `ioctl` | TTY, DevFS | Terminal window size & modes |
| 17 | `pread64` | Musl libc, bcache | Positional file read |
| 18 | `pwrite64` | Musl libc | Positional file write |
| 19 | `readv` | Musl stdio | Vectored buffer reads |
| 20 | `writev` | Musl stdio | Vectored buffer writes |
| 21 | `access` | BusyBox, TCC | File existence and permission checking |
| 22 | `pipe` | Musl libc, BusyBox sh | Creating IPC pipes |
| 23 | `select` | Musl libc | Multiplexed I/O |
| 24 | `sched_yield` | Musl libc | Relinquishing CPU time |
| 32 | `dup` | BusyBox sh | Duplicating file descriptors |
| 33 | `dup2` | BusyBox sh | Redirection (`>`, `<`) |
| 35 | `nanosleep` | BusyBox sleep | PIT/RTC backed high-resolution sleep |
| 39 | `getpid` | Musl libc, all binaries | Process identification |
| 41 | `socket` | Net stack, Musl | AF_UNIX and AF_INET socket creation |
| 42 | `connect` | Intent bus, Net stack | Connecting to UNIX/INET endpoints |
| 43 | `accept` | Net stack | Accepting socket connections |
| 44 | `sendto` | Net stack | UDP / UNIX datagram transmission |
| 45 | `recvfrom` | Net stack | UDP / UNIX datagram reception |
| 56 | `clone` | Musl pthread/fork | Process creation |
| 57 | `fork` | Musl libc, BusyBox sh | Process forking |
| 59 | `execve` | Musl libc, BusyBox sh | Executing ELF64 and #! scripts |
| 60 | `exit` | Musl libc | Task termination |
| 61 | `wait4` | Musl libc, BusyBox sh | Child process reaping |
| 63 | `uname` | BusyBox uname | Returning `6.1.0-myos x86_64` |
| 72 | `fcntl` | Musl libc, BusyBox | File descriptor flags (F_GETFL, F_SETFL, FD_CLOEXEC) |
| 74 | `fsync` | Musl libc, BusyBox | Flushing buffer cache to block device |
| 76 | `truncate` | Musl libc, sysfuzz | File truncation |
| 77 | `ftruncate` | Musl libc | Open file truncation |
| 79 | `getcwd` | Musl libc, BusyBox pwd | Current working directory query |
| 80 | `chdir` | Musl libc, BusyBox cd | Changing current directory |
| 83 | `mkdir` | Musl libc, BusyBox mkdir | Directory creation |
| 84 | `rmdir` | Musl libc, BusyBox rmdir | Directory removal |
| 87 | `unlink` | Musl libc, BusyBox rm | File unlinking |
| 88 | `symlink` | Musl libc, BusyBox ln -s | Symbolic link creation |
| 89 | `readlink` | Musl libc, BusyBox readlink | Reading symlink target |
| 90 | `chmod` | Musl libc, BusyBox chmod | File mode attribute modification |
| 95 | `umask` | Musl libc, BusyBox | File mode creation mask |
| 96 | `gettimeofday` | Musl libc, MicroPython | Real-time clock wall time |
| 97 | `getrlimit` | Musl libc | Resource limits query |
| 99 | `sysinfo` | Musl libc, BusyBox | System statistics query |
| 102 | `getuid` | Musl libc | User ID (0 / root) |
| 104 | `getgid` | Musl libc | Group ID (0 / root) |
| 107 | `geteuid` | Musl libc | Effective user ID |
| 108 | `getegid` | Musl libc | Effective group ID |
| 109 | `setpgid` | Musl libc, BusyBox sh | Process group management |
| 110 | `getppid` | Musl libc | Parent process ID |
| 158 | `arch_prctl` | Musl libc runtime | Setting FS base (`ARCH_SET_FS`) for TLS |
| 160 | `setrlimit` | Musl libc | Resource limits modification |
| 186 | `gettid` | Musl libc | Thread ID |
| 200 | `tkill` | Musl libc | Target thread signalling |
| 202 | `futex` | Musl libc, POSIX threads | Fast user mutex synchronization (WAIT, WAKE, WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE) |
| 217 | `getdents64` | Musl opendir/readdir | Directory entry enumeration |
| 218 | `set_tid_address`| Musl libc initialization | Clear child TID on thread exit |
| 228 | `clock_gettime` | Musl libc, MicroPython | Monotonic and real-time clock |
| 231 | `exit_group` | Musl libc `exit()` | Terminating thread group |
| 234 | `tgkill` | Musl libc | Thread group signalling |
| 257 | `openat` | Musl libc, BusyBox | Directory file descriptor relative open |
| 258 | `mkdirat` | Musl libc | Directory relative mkdir |
| 262 | `newfstatat` | Musl libc stat | Directory relative file stat |
| 263 | `unlinkat` | Musl libc | Directory relative file unlink |
| 264 | `renameat` | Musl libc | Directory relative file rename |
| 267 | `readlinkat` | Musl libc | Directory relative readlink |
| 269 | `faccessat` | Musl libc | Directory relative access check |
| 273 | `set_robust_list`| Musl libc, POSIX threads | Robust futex list pointer registration |
| 274 | `get_robust_list`| Musl libc, POSIX threads | Robust futex list pointer retrieval |
| 280 | `utimensat` | Musl libc, BusyBox touch | High-resolution timestamp modification |
| 293 | `pipe2` | Musl libc | Pipe with O_CLOEXEC / O_NONBLOCK |
| 302 | `prlimit64` | Musl libc | 64-bit resource limit control |
| 318 | `getrandom` | Musl libc, CSPRNG | Kernel CSPRNG random bytes |
| 332 | `statx` | Musl libc (stub) | Extended file metadata |

---

## Futex Subsystem & Timer Granularity

### Architecture & Supported Operations
MyOS implements a 64-bucket hashed waitqueue futex subsystem supporting:
- `FUTEX_WAIT` / `FUTEX_WAIT_BITSET`: Suspends caller if `*uaddr == val` until waken or timeout. Supports both relative `struct timespec` and absolute timestamps with `FUTEX_CLOCK_REALTIME`.
- `FUTEX_WAKE` / `FUTEX_WAKE_BITSET`: Wakes up to `val` waiters matching the 32-bit bitset mask.
- `FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE`: Atomically checks `*uaddr == val3`, wakes up to `val` waiters, and moves remaining waiters to secondary queue `uaddr2` (essential for POSIX condition variable broadcast without thundering herd).
- `set_robust_list` / `get_robust_list`: Registers the per-thread robust futex head. Upon unexpected thread termination (crash, SIGKILL, or `exit_group`), the kernel walks the robust list, marks the held futex words with `FUTEX_OWNER_DIED`, and wakes pending waiters with `futex_wake_all`.

### Timeout Granularity (100Hz = 10ms)
- **Clock Resolution**: The underlying kernel scheduler runs on the PIT timer configured at 100Hz (1 tick = 10 milliseconds).
- **Timeout Conversions**:
  $$\text{ticks} = \text{tv\_sec} \times 100 + \left\lceil\frac{\text{tv\_nsec}}{10,000,000}\right\rceil$$
- **Impact on Runtimes**:
  - `pthread_cond_timedwait`: Any sub-10ms timeout requested by userland will round up to a minimum of 1 tick (10ms). Deadlines are guaranteed to not fire early, but may experience up to 10ms jitter.
  - **Go Runtime (`time.Sleep` / channel timeouts)**: Go's timer wheel uses nanotime readings from `clock_gettime(CLOCK_MONOTONIC)` which reads hardware TSC cycles and is nanosecond-accurate. However, when Go blocks in the kernel via `futex(..., timeout)`, wakeups occur on 10ms tick boundaries unless woken by an explicit waker thread.
  - Future improvement: An APIC one-shot high-resolution timer path will be introduced with SMP (Phase 11) to provide sub-millisecond sleeping wait resolution.

---

## MyOS-Specific Syscalls (1000+)

MyOS defines custom extensions for agent sandboxing, semantic memory, intent bus, and audit hash chains in the reserved range starting at 1000:

| Number | Name | Description |
|---|---|---|
| 1000 | `SYS_TICKS` | Returns system timer tick count |
| 1001 | `SYS_FREE_FRAMES` | Returns number of free physical memory frames |
| 1002 | `SYS_AGENT_CREATE` | Allocates new agent sandbox with capability bitmask & resource budgets |
| 1003 | `SYS_AGENT_ATTACH` | Attaches current process to an existing sandboxed agent |
| 1004 | `SYS_AGENT_KILL` | Immediately halts and cleans up all processes in an agent sandbox |
| 1005 | `SYS_AGENT_SUSPEND` | Freezes all threads belonging to an agent |
| 1006 | `SYS_AGENT_RESUME` | Unfreezes suspended agent threads |
| 1007 | `SYS_AGENT_INFO` | Retrieves runtime CPU/memory/token usage for an agent |
| 1008 | `SYS_SOCK_PEERINFO` | Queries peer PID and agent ID on AF_UNIX connection |
| 1009 | `SYS_BUS_DAEMON_REGISTER` | Registers the system Intent Bus daemon (`intentd`) |
| 1010 | `SYS_AUDIT_APPEND` | Appends entry to SHA-256 tamper-evident kernel audit log |
| 1011 | `SYS_AGENT_CHARGE_TOKENS` | Atomically deducts LLM tokens from agent budget |
| 1012 | `SYS_AGENT_TOKENS_LEFT` | Queries remaining LLM token allocation |
| 1013 | `SYS_MODEL_DAEMON_REGISTER` | Registers the system Model daemon (`modeld`) |
| 1014 | `SYS_AGENT_KILLSWITCH_STATE` | Queries hardware/emergency killswitch status |
| 1015 | `SYS_AGENT_CHARGE_NET` | Deducts outbound network bytes from agent budget |
| 1016 | `SYS_AGENT_NET_LEFT` | Queries remaining network byte budget |
| 1017 | `SYS_NET_DAEMON_REGISTER` | Registers the system Network Gateway daemon (`netd`) |
| 1018 | `SYS_TEST_HARDENING` | Triggers test-only SMEP/SMAP/fault validation |
| 1020 | `SYS_TCGETATTR` | Legacy terminal configuration get |
| 1021 | `SYS_TCSETATTR` | Legacy terminal configuration set |
| 1099 | `SYS_TEST_KSTACK_OVERFLOW` | Triggers infinite recursion to verify kernel stack guard page fault |

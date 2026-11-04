# MyOS Automated Test Matrix

> **Source of Truth**: This document is generated automatically by `tools/gen_test_matrix.sh` directly from execution of `tools/test_boot.sh`. Do not edit by hand.

## Test Sessions Execution Matrix

| Session | Title | Status |
|:---|:---|:---:|
| **Session 1** | Booting and testing commands, VFS, utilities, and regressions | **PASS** |
| **Session 2** | Persistence test across QEMU reboot with the SAME disk.img | **PASS** |
| **Session 3** | Stage 14 IPC, pipes, redirections, pipelines, filters, and stress test | **PASS** |
| **Session 4** | Host-side FAT filesystem integrity check | **PASS** |
| **Session 5** | Low-memory boot test (-m 64) | **PASS** |
| **Session 6** | Live job control and interactive TTY signals over COM1 | **PASS** |
| **Session 7** | Stage 16 Time, RTC, sleeping wait queue, procfs, scheduler, and responsiveness | **PASS** |
| **Session 8** | Testing Newlib libc (libctest) and Lua 5.4 runtime | **PASS** |
| **Session 9** | Stage 18A /dev/fb0 mmap, mouse driver, and keyboard input layer | **PASS** |
| **Session 10** | Stage 18A poll(), select(), non-blocking I/O, and libc flag translation tests | **PASS** |
| **Session 11** | Stage 18B SHM, UNIX domain sockets, PTY subsystem, and terminal emulator | **PASS** |
| **Session 12** | Stage 18B memtest suite (fork storm, sbrk, mmap/munmap, zero frame leaks) | **PASS** |
| **Session 13** | Stage 18B crash safety, invalid pointer, page fault, and non-regression tests | **PASS** |
| **Session 14** | Stage 18B SMP-ready concurrency and multi-threaded stress tests | **PASS** |
| **Session 15** | Stage 18B LFN stress test (nested dirs, alias collision, deep paths, zero frame leaks) | **PASS** |
| **Session 16** | Kernel Agents (Capabilities, Delegation, Budgets, Audit Hash Chain, agentctl, -m 64 boot) | **PASS** |
| **Session 17** | Intent Bus (intentd, libjson, libintent, peer identity, demo skills, approvals, intentctl, -m 64 boot) | **PASS** |
| **Session 18** | Model Daemon, host bridge, token budgets, and agent loop | **PASS** |
| **Session 19** | The Agent Desktop, accessibility tree, window capabilities, trusted approval UI, and killswitch | **PASS** |
| **Session 20** | Persistent agent memory (principals, memd, memctl) and semantic file index (indexd) | **PASS** |
| **Session 21** | Real networking, VirtIO net, TLS, and Agent-Safe Network Gateway | **PASS** |
| **Session 22** | x86-64 Long Mode, UEFI Boot, >4GB RAM, and CPU Hardening Suite | **PASS** |
| **Session 23** | Linux ABI Compatibility, Musl Libc, BusyBox, Lua, Python, TCC Self-Hosting | **PASS** |

---

## Verification Methodology
- Every session boots inside a hermetic QEMU x86-64 long-mode machine with serial I/O logging.
- Memory leak checks assert baseline versus final free physical frames within strict tolerances.
- Filesystem consistency is checked with host-side `fsck.fat -v -n disk.img` after dirty reboots and poweroffs.
- Capability violations, CPU time budget overruns, and token budget limits are strictly audited against cryptographic SHA-256 hash chains.

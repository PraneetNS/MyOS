# Stage 18B: Kernel IPC & Session Infrastructure for a Window System

## Overview
Stage 18B introduces the foundational kernel IPC and session management primitives required for a multi-window display server and multi-process desktop environment on MyOS:
1. **tmpfs & Flexible Mounts**: In-memory filesystem (`tmpfs`) mounted at `/dev/shm` and `/run`, supporting page-backed sparse allocation, `truncate`/`ftruncate`, `S_IFSOCK`, `S_IFCHR`, directory trees, and `/proc/mounts`.
2. **Shared Memory (`MAP_SHARED`)**: Unified page-table-level shared memory supporting anonymous mappings (`MAP_SHARED | MAP_ANON`), tmpfs file-backed mappings, bidirectional synchronization across `fork()`, zero-filled sparse holes, beyond-EOF `SIGBUS` enforcement, `shm_open`/`shm_unlink`, and `msync`.
3. **UNIX Domain Sockets (`AF_UNIX`)**: Bidirectional byte-stream IPC (`SOCK_STREAM`) with 64KB kernel ring buffers, tmpfs filesystem binding (`S_IFSOCK`), `socketpair()`, `bind()`, `listen()`, `accept()`, `connect()`, `shutdown()`, `/proc/net/unix`, and `/proc/<pid>/fd` descriptor inspection.
4. **Pseudoterminals (PTY) & Multiple TTYs**: UNIX98 PTY master multiplexer `/dev/ptmx`, dynamic devfs slave nodes `/dev/pts/<N>`, bidirectional line discipline (canonical mode, `ICRNL`, `ECHO`, `ONLCR`), `TIOCSWINSZ`/`TIOCGWINSZ`, `SIGWINCH` delivery, `posix_openpt()`, `grantpt()`, `unlockpt()`, `ptsname_r()`, `openpty()`, `forkpty()`, virtual consoles `/dev/tty0`..`/dev/tty3`, and kernel cmdline `serial_shell=1` support.
5. **Init, Sessions, & Power Management**: `/sbin/init` (PID 1) parsing `/etc/init.conf` with `once`, `wait`, and `respawn` directives, rate-limiting (max 5 restarts per 10s with 5s backoff), orphan reaping via `waitpid(-1)`, `/proc/cmdline` integration, and `SYS_REBOOT` poweroff, reboot, and halt with sync flushing.
6. **Comprehensive Verification**: Dedicated test suites (`shmtest.elf`, `sockettest.elf`, `ptytest.elf`, `ipctest.elf`), stress tests, tick-loss verification, dirty buffer poweroff verification, and FAT filesystem consistency checks.

---

## 1. tmpfs Architecture & Mount Infrastructure

### Mount Hierarchy & Configuration
- **Mount Point Table**: `src/mount.c` maintains active filesystem mounts. `/` is mounted as FAT16, `/dev` as DevFS, `/proc` as ProcFS, `/dev/shm` as tmpfs (63MB cap), and `/run` as tmpfs (63MB cap).
- **`/proc/mounts`**: Exposes the active mount table in standard `/etc/mtab` format:
  ```text
  /dev/hda / fat16 rw,relatime 0 0
  devfs /dev devfs rw,nosuid,noexec,relatime 0 0
  procfs /proc procfs rw,nosuid,nodev,noexec,relatime 0 0
  tmpfs /dev/shm tmpfs rw,nosuid,nodev,relatime,size=65404k 0 0
  tmpfs /run tmpfs rw,nosuid,nodev,relatime,size=65404k 0 0
  ```

### tmpfs Node & Memory Management
- **Inode Representation**: `tmpfs_node_t` represents an in-memory file, directory, or socket.
- **Sparse Page Array**: Regular files store physical page frame numbers in a dynamically expanding array.
  - Initial pages are unallocated (sparse holes).
  - Unallocated pages read back as zeros without consuming physical memory.
  - Touched pages allocate real frames via `pmm_alloc_frame()`.
- **Truncation (`ftruncate`)**:
  - Growing a file updates `st_size` without allocating unused intermediate frames.
  - Shrinking a file deallocates frames beyond the new size back to `pmm` and invalidates any active PTEs in processes mapping those pages.
- **Node Types**:
  - `TMPFS_REGULAR`: Standard data files and shared memory objects.
  - `TMPFS_DIR`: Directory containing a linked list of directory entries.
  - `TMPFS_SOCKET`: `S_IFSOCK` filesystem rendezvous nodes created by `bind()`.
  - `TMPFS_CHARDEV`: Character device nodes (`S_IFCHR`).

---

## 2. Shared Memory (`MAP_SHARED`)

### Memory Mapping Architecture
- **Anonymous Shared (`MAP_SHARED | MAP_ANON`)**:
  - Backed by an anonymous frame collection managed by the virtual memory subsystem.
  - Upon `fork()`, child processes map the *identical* physical frame numbers with `PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE` (no Copy-On-Write).
  - Cache and atomics (`__atomic_fetch_add`, `lock cmpxchg`) operate directly across processes.
- **File-Backed Shared (`MAP_SHARED` on tmpfs)**:
  - Backed by the underlying `tmpfs_node_t` page table.
  - Read/write access via pointer modifies the tmpfs file contents immediately.
  - Syscalls (`read()`, `write()`, `lseek()`) interact directly with the same physical frames.
  - FAT filesystem rejection: Attempting `MAP_SHARED` on a FAT file returns `-ENODEV` because FAT lacks in-kernel coherent page-cache writeback.
- **Beyond-EOF Access & `SIGBUS`**:
  - If a process maps a tmpfs file for $N$ pages but the file length is smaller than the mapped range, touching addresses beyond `st_size` generates a page fault resulting in a `SIGBUS` signal (signal 7).
  - If an active mapping exists and `ftruncate()` shrinks the file below mapped pages, subsequent accesses to truncated pages raise `SIGBUS`.
- **POSIX SHM Utilities**:
  - `shm_open(name, oflag, mode)`: Opens or creates a file under `/dev/shm/<name>`.
  - `shm_unlink(name)`: Removes `/dev/shm/<name>` from tmpfs.
  - `msync(addr, length, flags)`: Validates memory range and returns 0.

---

## 3. AF_UNIX Stream Sockets

### Socket Implementation & State Machine
- **Domain & Type**: `socket(AF_UNIX, SOCK_STREAM, 0)`.
- **States**: `UNIX_STATE_CLOSED`, `UNIX_STATE_BOUND`, `UNIX_STATE_LISTENING`, `UNIX_STATE_CONNECTED`, `UNIX_STATE_DISCONNECTED`.
- **Ring Buffers**:
  - Each connected socket possesses an incoming 64KB ring buffer (`UNIX_SOCKET_BUF_SIZE = 65536`).
  - Writes copy data into the peer's incoming ring buffer.
  - Full buffers cause writers to block on `peer->write_wait` channel until reader drains data.
  - Empty buffers cause readers to block on `sock->read_wait` channel until writer produces data.
  - Non-blocking sockets (`O_NONBLOCK`) return `-EAGAIN` when buffers cannot be immediately read or written.
- **Filesystem Rendezvous (`bind`)**:
  - Sockets can bind to paths located on `tmpfs` mounts (e.g., `/run/display.sock`).
  - Binding creates an `S_IFSOCK` directory entry pointing to the server socket.
  - Attempting to bind to a FAT filesystem returns `-EOPNOTSUPP`.
  - Attempting to bind an existing path returns `-EADDRINUSE`.
- **Connection Handshake**:
  - `listen(fd, backlog)`: Marks the socket as listening with a queue of pending connections.
  - `connect(fd, addr, len)`: Locates the server socket, allocates a client-server socket pair, and enqueues the client to `srv->pending_connections`.
  - `accept(fd, addr, len)`: Pops a connected peer socket and allocates a new file descriptor.
- **Shutdown & Error Semantics**:
  - `shutdown(fd, SHUT_RD)`: Disables reading; subsequent reads return 0 (EOF).
  - `shutdown(fd, SHUT_WR)`: Sends EOF to the peer; subsequent writes return `-EPIPE` and deliver `SIGPIPE`.
  - Closed peer returns `-EPIPE` / `SIGPIPE` on write.
- **ProcFS Visibility**:
  - `/proc/net/unix`: Displays active UNIX sockets, refcounts, protocol states, and bound paths:
    ```text
    Num       RefCount Protocol Flags    Type St Path
    00000000: 00000002 00000000 00010000 0001 01 /run/ipctest.sock
    ```
  - `/proc/<pid>/fd`: Directory containing numbered symlinks/nodes for every open descriptor held by PID.

---

## 4. Pseudoterminal Subsystem (PTY) & Multiple TTYs

### Master/Slave Architecture
```
  [ Terminal Emulator / UI Server ]
                 |  read / write
                 v
          /dev/ptmx (Master)
                 |
      +----------v-----------+
      |  Line Discipline     |  (Canonical mode, ICRNL,
      |  & Ring Buffers      |   ONLCR, ECHO, ISIG)
      +----------+-----------+
                 |
          /dev/pts/N (Slave)
                 ^
                 |  read / write / ioctl
  [ Shell / Client Process (sh, lua) ]
```

- **/dev/ptmx Multiplexer**:
  - Opening `/dev/ptmx` allocates an unused PTY structure from `ptys[MAX_PTYS]`.
  - Master file descriptor allows the host process (e.g. terminal emulator or display server) to simulate a physical terminal.
- **/dev/pts/<N> Slaves**:
  - Granting (`grantpt`) and unlocking (`unlockpt`) enables the slave device.
  - `ptsname_r(master_fd, buf, len)` queries the slave path via `TIOCGPTN`.
  - Dynamically resolved through DevFS (`/dev/pts/0`, `/dev/pts/1`, etc.).
- **Line Discipline**:
  - Master writes are fed to `tty_input_char()`. In canonical mode, input is line-buffered; `\r` is converted to `\n` via `ICRNL`.
  - Master receives echoed characters when `ECHO` is enabled, formatted with `\r\n` via `ONLCR`.
  - Slave writes go directly into the master read ring buffer.
- **Window Size & Signals**:
  - `TIOCSWINSZ` on master sets `struct winsize` (rows, cols, xpixel, ypixel) and sends `SIGWINCH` to the slave's foreground process group.
  - `TIOCGWINSZ` on slave allows clients to read current terminal dimensions.
  - Closing the master delivers `SIGHUP` and `SIGCONT` to the slave foreground process group and wakes any blocked readers.
- **Helper APIs**:
  - `posix_openpt(flags)`: Opens `/dev/ptmx`.
  - `openpty(amaster, aslave, name, termp, winp)`: Allocates master/slave pair and configures attributes.
  - `forkpty(amaster, name, termp, winp)`: Creates a child session (`setsid`), binds slave as controlling terminal (`TIOCSCTTY`), duplicates slave to fd 0, 1, 2, and returns child PID to parent.
- **Virtual Consoles & Kernel Cmdline**:
  - `/dev/tty0` .. `/dev/tty3` exist as DevFS nodes.
  - Passing `serial_shell=1` on the boot cmdline routes the initial shell to `/dev/ttyS0` (COM1) instead of `/dev/console`.

---

## 5. Init (PID 1), Sessions, & Power Management

### `/sbin/init` Architecture
- **Spawning PID 1**: Kernel spawns `/sbin/init` as the first userland process.
- **Configuration Parsing (`/etc/init.conf`)**:
  - Lines follow `<action> <tty_device> <command> [args...]`:
    ```text
    # /etc/init.conf - MyOS init configuration
    respawn /dev/console /bin/sh.elf
    ```
  - **`once`**: Executes command once in the background.
  - **`wait`**: Executes command and halts init execution until command exits.
  - **`respawn`**: Relaunches command automatically whenever it terminates.
- **Rate-Limiting Protection**:
  - If a respawn process crashes more than 5 times within a 10-second sliding window, init enters a 5-second backoff penalty before restarting, preventing CPU starvation from broken binaries.
- **Session & Controlling Terminal**:
  - For each spawned process, init opens `<tty_device>`, calls `setsid()`, binds controlling terminal with `ioctl(tty_fd, TIOCSCTTY, 0)`, sets foreground process group with `ioctl(tty_fd, TIOCSPGRP, &pid)`, and redirects fd 0, 1, 2.
- **Orphan Reaping**:
  - When any process parent dies, `scheduler_exit_process()` reparents the orphaned child to PID 1 (`child->ppid = 1;`).
  - Init loops on `waitpid(-1, &status, WNOHANG)` to continuously reap zombie processes, preventing PID leaks.

### System Reboot & Poweroff (`SYS_REBOOT`)
- **Syscall Number**: 88 (`SYS_REBOOT`).
- **Linux Magic Verification**: Requires `magic1 == 0xfee1dead` and `magic2 == 672274793` (`0x28121969`).
- **Commands**:
  - `RB_POWER_OFF` (`0x4321fedc`): Flushes buffer cache, terminates user processes, and triggers ACPI/QEMU poweroff via I/O ports `0x604=0x2000`, `0xB004=0x2000`, `0x4004=0x3400`, `0x600=0x34`.
  - `RB_AUTOBOOT` (`0x01234567`): Flushes buffer cache and pulses 8042 keyboard controller reset line (`0x64=0xFE`) or triggers triple fault.
  - `RB_HALT_SYSTEM` (`0xcdef0123`): Flushes buffer cache and enters infinite `cli; hlt` loop.
- **Graceful Process Shutdown**:
  - `g_kernel_shutting_down = 1` flag prevents PID 1 exit panic.
  - Broadcasts `SIGTERM` and `SIGKILL` to all active user processes.
  - Re-enables interrupts (`asm volatile ("sti")`) to allow PIT timer ticks to advance while waiting for processes to terminate.
  - Calls `vfs_sync()` and `bcache_flush_all()` before hardware reset or poweroff.

---

## 6. Syscall Reference Table (Stage 18B Updates)

| Syscall | Number | Arguments | Description |
|---|---|---|---|
| `SYS_SOCKET` | 71 | `domain, type, protocol` | Create an AF_UNIX stream socket |
| `SYS_BIND` | 72 | `sockfd, sockaddr, addrlen` | Bind socket to a tmpfs `S_IFSOCK` path |
| `SYS_LISTEN` | 73 | `sockfd, backlog` | Set socket to listening mode |
| `SYS_ACCEPT` | 74 | `sockfd, sockaddr, addrlen` | Accept incoming connection on listening socket |
| `SYS_CONNECT` | 75 | `sockfd, sockaddr, addrlen` | Connect to a remote socket |
| `SYS_SHUTDOWN` | 76 | `sockfd, how` | Shut down read, write, or both halves of socket |
| `SYS_SOCKETPAIR` | 77 | `domain, type, protocol, sv[2]` | Create an unnamed connected socket pair |
| `SYS_TRUNCATE` | 85 | `path, length` | Truncate file by path |
| `SYS_FTRUNCATE` | 86 | `fd, length` | Truncate open file by descriptor |
| `SYS_MSYNC` | 87 | `addr, length, flags` | Synchronize memory mapping |
| `SYS_REBOOT` | 88 | `magic1, magic2, cmd, arg` | Poweroff, reboot, or halt system |

---

## 7. Concurrency & Locking Hierarchy

To prevent deadlocks across filesystem operations, socket transfers, and PTY line discipline, the following strict locking order is enforced:
1. **`vfs_lock`** (Global VFS hierarchy mutex)
2. **`vnode->lock`** (Individual file/directory node lock)
3. **`tmpfs_node->lock`** (tmpfs inode structure lock)
4. **`unix_socket->lock`** (Socket ring buffer and state lock)
5. **`pty->lock`** (PTY master/slave pair lock)
6. **`bcache_lock`** (Low-level block cache buffer lock)

**Rules:**
- Never sleep or acquire mutexes within hardware ISR handlers (PIT, serial COM1, PS/2 mouse/keyboard).
- All blocking wait channels (`scheduler_wait_channel`) must release held mutexes before context-switching and re-acquire upon awakening.
- Mutexes are automatically released on process termination in `scheduler_exit_process()`.

---

## 8. Verification & Test Programs

1. **`shmtest.elf`**:
   - `MAP_SHARED | MAP_ANON` with 4 concurrent child processes incrementing an atomic counter 100,000 times each (verifying exact 500,000 total).
   - FAT filesystem `MAP_SHARED` rejection (`-ENODEV`).
   - tmpfs named shared memory cross-process synchronization and `write()`/`read()` coherence.
   - `ftruncate` sparse expansion, shrinkage, and `SIGBUS` beyond-EOF verification.
2. **`sockettest.elf`**:
   - Protocol validation (AF_INET and SOCK_DGRAM rejection).
   - tmpfs filesystem binding, `S_IFSOCK` node creation, `EADDRINUSE` duplicate check.
   - 128KB ring buffer wrap stress testing bidirectional transfer.
   - `shutdown(SHUT_WR)` and `SIGPIPE` delivery.
   - Multi-client server with `poll()` multiplexing.
3. **`ptytest.elf`**:
   - `/dev/ptmx` master allocation, `grantpt`, `unlockpt`, and `ptsname_r`.
   - Canonical line discipline (`\r` -> `\n`) and echo verification.
   - `TIOCSWINSZ` window resize and `SIGWINCH` delivery.
   - `openpty()` and `forkpty()` child session and I/O streams.
4. **`ipctest.elf`**:
   - End-to-end integration test exercising tmpfs, shm atomics, AF_UNIX stream sockets, PTYs, and multi-process pipelines.
5. **System Validation**:
   - `poweroff` with pending filesystem writes followed by `fsck.fat -v -n disk.img` verification.
   - `reboot` execution with QEMU termination.
   - 20-second PIT tick loss verification (`tools/test_concur_suite.py`).
   - Frame allocation tracking confirming zero physical frame leaks.

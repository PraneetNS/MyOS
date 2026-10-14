# FAT16 Long Filename (LFN) Architecture & Implementation Plan

## Pre-Step 2: Kernel Concurrency, Static Buffer Audit & Stack Analysis

### 1. Inventory of Kernel Buffers Moved to Static/BSS Storage
Prior to this audit, several large buffers and tracking states were stored in kernel static/BSS storage instead of userland or dynamic allocation:
1. **`src/syscall.c` (`SYS_EXECVE` / `SYS_EXEC`)**:
   - `static char kname[PATH_MAX]` (1,024 bytes)
   - `static char bin_path[PATH_MAX]` (1,024 bytes)
   - `static char kargv_buf[16][NAME_MAX + 1]` (4,096 bytes)
   - `static char kenvp_buf[32][128]` (4,096 bytes)
   - *Total*: 10,240 bytes (~10 KB) of static buffers in kernel text/BSS.
2. **`src/vfs.c`**:
   - `static int current_op_flags`: Global variable capturing open flags for the active read/write operation.
   - `static vnode_t vnode_pool[MAX_VNODES]` and `static open_file_t of_pool[MAX_OPEN_FILES]`: Global fixed tables for VFS objects.
3. **`src/fat16.c`**:
   - `static fat_node_t node_pool[128]`: Global fixed array of FAT inode structures without a corresponding deallocation mechanism.
4. **`src/bcache.c`**:
   - `static bcache_entry_t cache[64]`: Global block cache table and sector buffers.

### 2. Concurrency & Data Race Analysis
In a preemptive multitasking kernel with a 100 Hz timer interrupt, I/O waits, and scheduler preemption:
- **`SYS_EXECVE` static buffers**:
  - *Can two processes enter concurrently?* **YES.** `SYS_EXECVE` invokes `vfs_resolve_path()` and `vnode->ops->read()` to parse ELF headers. Disk operations yield to the scheduler during ATA PIO or block cache operations, and timer preemption can switch processes mid-syscall. If Process B calls `exec` while Process A is reading its ELF binary from disk, Process B will clobber `kname`, `bin_path`, `kargv_buf`, and `kenvp_buf`. This corrupts Process A's command-line arguments, environment pointers, and loaded program image, causing silent crashes or executing wrong binaries.
- **`src/vfs.c` (`current_op_flags`)**:
  - *Can two processes enter concurrently?* **YES.** Process A reading `/dev/input/kbd0` or a pipe with `O_NONBLOCK` can be preempted by Process B performing a blocking read on another file. Process B overwrites `current_op_flags`, causing Process A's non-blocking I/O to unexpectedly block, or Process B's blocking I/O to return `EAGAIN` prematurely.
- **`src/vfs.c` (`vnode_pool` & `of_pool`)**:
  - *Can two processes enter concurrently?* **YES.** Preemption during `open()`, `close()`, `vnode_alloc()`, or refcount adjustments causes double allocations and lost refcounts. Furthermore, `vfs_resolve_path()` previously leaked intermediate vnode references during multi-component path resolution.
- **`src/fat16.c` (`node_pool`)**:
  - *Can two processes enter concurrently?* **YES.** Without concurrency protection, concurrent lookups race on entry allocation. Moreover, the lack of freeing resulted in permanent pool exhaustion after 128 unique lookups.
- **`src/bcache.c` (`cache`)**:
  - *Can two processes enter concurrently?* **YES.** ATA PIO read/write loops (`inw`/`outb`) take multiple milliseconds. A timer interrupt occurring mid-transfer allows another thread to issue ATA commands, corrupting the hardware ATA state machine and cache slot metadata.

### 3. Concurrency Fixes & Kernel Stack Headroom
1. **Dynamic Heap Allocation for Syscall Buffers**:
   - In `src/syscall.c`, static buffers in `SYS_EXEC` were replaced with dynamically allocated `exec_params_t* ep = kmalloc(sizeof(exec_params_t))` with explicit `kfree(ep)` on all exit paths.
2. **Per-Process Scratch & State Tracking**:
   - In `src/process.h` and `src/vfs.c`, `current_op_flags` was moved into `process_t` (`me->current_op_flags`). Each process tracks its own operational flags safely.
3. **Synchronization & Lifecycle Cleanups**:
   - `src/vfs.c`: Wrapped `vnode_pool` and `of_pool` mutations in `vfs_lock()`/`vfs_unlock()` (`pushf; cli; popf`). Resolved intermediate vnode leaks in `vfs_resolve_path()`. Added `cleanup` callback to `struct fs_ops` invoked on final `vnode_unref()`.
   - `src/bcache.c`: Wrapped `bcache_read()`, `bcache_write()`, and `bcache_sync()` in `bcache_lock()`/`bcache_unlock()` to serialize ATA PIO transfers and cache slot updates.
   - `src/fat16.c`: Converted `node_pool` to per-node dynamic `kmalloc`/`kfree` (`fat16_cleanup`). Added locked wrapper functions (`fat16_locked_*`) with `fat_lock()`/`fat_unlock()`. Zeroed new cluster sectors on directory extension in `alloc_entry_in_dir()`. Fixed self-deletion edge case in `fat16_rename()`.
4. **Kernel Stack Size & Call Chain Analysis**:
   - **Kernel Stack Size**: Increased from 8,192 bytes (8KB) to **16,384 bytes (16KB)** per process in `src/process.h` (`PROC_KERNEL_STACK_SIZE = 16384`).
   - **Deepest Call Chain**:
     ```
     syscall_handler (interrupt frame: 76 bytes)
       -> sys_open / vfs_resolve_parent (frame: ~320 bytes)
         -> vfs_resolve_path (frame: ~1,216 bytes)
           -> vfs_path_canonical (frame: ~1,792 bytes)
             -> fat16_lookup (frame: ~256 bytes)
               -> find_entry_in_dir (frame: ~128 bytes)
                 -> read_dir_entry (frame: ~544 bytes)
                   -> bcache_read (frame: ~544 bytes)
                     -> ata_read_sectors (frame: ~64 bytes)
     ```
   - **Peak Stack Depth**: **~4,940 bytes (~4.8 KB)**.
   - **Headroom**: With a 16KB stack, maximum stack utilization is **~30.1%**, leaving **>11.4 KB** of safe headroom for nested interrupt service routines (timer, keyboard, mouse, ATA) and exception frames.

### 4. Concurrent Stress Test (`userland/concur_test.c`)
- **Test Setup**:
  - 4 concurrent worker processes performing 10 iterations each on deep nested paths (`/cstress/wX/sub1/sub2/f_origY.txt`).
  - Operations per iteration: `mkdir`, `open(O_CREAT|O_WRONLY|O_TRUNC)`, `write`, `stat`, `opendir`, `readdir`, `closedir`, `rename`, `stat`, `open(O_RDONLY)`, `read` (payload data verification), `unlink`, `rmdir`.
  - A 5th background process (spinner) runs a continuous tight loop, guaranteeing 100 Hz timer preemption fires across all VFS, FAT, bcache, and ATA routines.
- **Verification Results**:
  - `concur_test` completed all iterations across all 4 workers with zero errors.
  - Data integrity verified: payload matches exactly on every read, no path or data corruption.
  - Memory leak verification: physical frame count measured before and after stress phases confirmed **zero frame leaks** (`baseline = 64132`, `final = 64132`, `diff = 0`).
  - Host filesystem consistency: `fsck.fat -v -n disk.img` reported **clean with 0 errors** (`disk.img: 293 files, 13321/32695 clusters`).

### 5. Sleeping Mutex Conversion & Responsiveness Architecture

#### A. Lock Hierarchy and Conversion from CLI Spinlocks
Originally, short-term critical sections and long-held disk transfers both used interrupt-disabling CLI locks:
```c
static inline uint32_t spin_lock_irqsave(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void spin_unlock_irqrestore(uint32_t flags) {
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory");
}
```
Holding CLI across long-running ATA PIO sector transfers (`inw`/`outb` loops taking several milliseconds) starved the 100 Hz PIT timer interrupt, causing dropped timer ticks and stalling serial/console Ctrl+C handling.

To eliminate latency without introducing data races:
1. **Short Critical Sections (`vfs_lock` / `vfs_unlock`)**:
   - Remain interrupt-disable spinlocks (`pushfl; popl; cli` and `pushl; popfl`) strictly for microsecond-scale non-blocking operations: pool slot allocation/freeing (`vnode_pool`, `of_pool`) and wait-queue manipulation.
2. **Long-Held Operations (`bcache_mutex`, `fat_mutex`)**:
   - Converted to sleeping FIFO mutexes (`kmutex_t`) in `src/kmutex.c`:
     ```c
     typedef struct kmutex {
         volatile int locked;
         int owner_pid;
         int wait_count;
         process_t* waiters[KMUTEX_MAX_WAITERS];
         const char* name;
     } kmutex_t;
     ```
   - When contested, the calling process is placed on the mutex's wait queue and calls `scheduler_wait_or_idle()`, yielding the CPU.
   - On release (`kmutex_unlock`), ownership is handed off directly to the head waiter, which is marked `PROC_READY`.
   - **ATA PIO Transfers Run with Interrupts Enabled**: `bcache_read()`, `bcache_write()`, and `bcache_sync()` now execute ATA commands and PIO data transfers with CPU interrupts fully enabled. Timer ticks, serial interrupts (COM1), and PS/2 mouse/keyboard events fire without interruption.
3. **Strict Lock Ordering**:
   - Deadlocks are prevented by enforcing a strict global lock hierarchy:
     $$\text{VFS Lock (CLI)} \rightarrow \text{FAT Mutex (Sleeping)} \rightarrow \text{Bcache Mutex (Sleeping)}$$
   - Never inverted; no sleeping mutex is ever acquired from an Interrupt Service Routine (ISR) or with interrupts disabled.
4. **Debug Sanity Assertions**:
   - `kmutex_assert_none_held()` is called at `syscall_handler` return and `process_destroy()` to guarantee no process leaks locks back into user space or upon termination.
   - Added user-mode interrupt flag assertion `((regs->eflags & 0x200) != 0)` verifying interrupts are never disabled when executing in user mode.

#### B. Timer Tick-Loss & Responsiveness Benchmarks

| Metric | Before (CLI Spinlocks) | After (Sleeping Mutexes) | Assertion / Requirement |
| :--- | :--- | :--- | :--- |
| **Timer Tick Rate under Heavy Disk I/O** | 72 - 86 ticks/sec (systematic loss) | **100 ticks/sec** (2,000 ticks in 20.0s RTC) | $98 - 102$ ticks/sec |
| **Serial / Ctrl+C Response Time** | > 1.8s (or delayed until I/O done) | **0.000s** (< 1.0s target) | Immediate SIGINT kill |
| **PS/2 Mouse Event Delivery** | Stalled / dropped packets | **100% events delivered** (`REL_X`, `REL_Y`, `BTN_LEFT`) | Zero lost events |
| **PMM Frame Leak Under Stress** | 0 frame leaks | **0 frame leaks** | Clean reclamation |
| **Host Filesystem Check (`fsck.fat`)** | Clean | **Clean (0 errors, 0 orphaned clusters)** | Bit-for-bit clean |

---

## 1. System Audit & Buffer Size Analysis

### Current Limitations (Pre-LFN)
1. **Name Limits & Directory Entries (`struct dirent`)**:
   - `src/vfs.h`: `struct dirent.d_name` was hardcoded to `char d_name[64]`.
   - `libmyos/include/dirent.h`: `struct dirent.d_name` was `char d_name[64]`.
   - Neither header defined standard POSIX `NAME_MAX` or `PATH_MAX`.
   - `src/fat16.c`: Directory entries only handled 8.3 FAT short names (`to_dos_name` and `from_dos_name`). VFAT entries with attribute `0x0F` (`FAT_ATTR_LFN`) were explicitly ignored during directory iteration (`readdir`), and creation/lookup relied on 8.3 truncation.

2. **Kernel Path & Token Buffers**:
   - `src/vfs.c`:
     - `vfs_resolve_path`: `canon[128]`, `token[64]`.
     - `vfs_resolve_parent`: `parent_path[128]`.
     - `vfs_path_canonical`: `combined[128]`, `segs[16][32]`. Any path segment exceeding 31 characters was truncated.
   - `src/syscall.c`:
     - `SYS_EXEC`: `kname[64]`, `bin_path[64]`.
     - `SYS_OPEN`, `SYS_MKDIR`, `SYS_RMDIR`, `SYS_UNLINK`: `child_name[64]`.
     - `SYS_RENAME`: `old_child[64]`, `new_child[64]`.
   - `src/process.h`:
     - `process_t.cwd[64]`: Current working directory buffer was only 64 bytes.

3. **Userland Tools**:
   - `userland/sh.c`: `line[256]`, `token_buf[512]`, `cwd[64]`.
   - `userland/ls.c`: `item_path[128]`.
   - `tools/build_disk.sh`: Generates short aliases (e.g. `screensh.elf`, `crash_sa.elf`) as workarounds for 8.3 limits.

---

## 2. Universal Limits & Error Handling

- **`NAME_MAX = 255`**: Maximum length of a single path component in UTF-8 bytes (excluding NUL).
- **`PATH_MAX = 1024`**: Maximum length of an absolute or relative path string (including NUL terminator).
- **`struct dirent`**: Expanded `d_name` from 64 to 256 bytes (`NAME_MAX + 1`).
- **`-ENAMETOOLONG`**:
  - Returned when any individual component length exceeds `NAME_MAX`.
  - Returned when total canonical or input path length exceeds `PATH_MAX`.

---

## 3. VFAT LFN Specifications & On-Disk Format

### VFAT Directory Entry Structure (32 bytes)
```c
typedef struct {
    uint8_t  seq;          /* Sequence number: 1..31 | 0x40 (LAST_LFN) */
    uint16_t name1[5];     /* Characters 1-5 (UTF-16LE) */
    uint8_t  attr;         /* Always 0x0F (FAT_ATTR_LFN = READONLY|HIDDEN|SYSTEM|VOLUME_ID) */
    uint8_t  type;         /* Reserved, always 0x00 */
    uint8_t  checksum;     /* Checksum of the associated 8.3 alias */
    uint16_t name2[6];     /* Characters 6-11 (UTF-16LE) */
    uint16_t fst_clus_lo;  /* Reserved, always 0x0000 */
    uint16_t name3[2];     /* Characters 12-13 (UTF-16LE) */
} __attribute__((packed)) fat_lfn_entry_t;
```

### Key VFAT Rules
1. **Reverse Order**: LFN entries precede their corresponding 8.3 short entry on disk in reverse order (entry with sequence `N | 0x40` comes first, followed by `N-1`, ..., `1`, then the short entry).
2. **Checksum**: The 8-bit checksum is calculated over the 11-byte short name:
   ```c
   uint8_t chk = 0;
   for (int i = 0; i < 11; i++) {
       chk = ((chk & 1) ? 0x80 : 0) + (chk >> 1) + (uint8_t)short_name[i];
   }
   ```
3. **Padding**: After the NUL terminator `0x0000`, unused UTF-16 character slots are padded with `0xFFFF`.
4. **NT Case Flags (Byte 12 / `nt_res`)**:
   - `0x08`: Base name is all lowercase.
   - `0x10`: Extension is all lowercase.
   When an 8.3 name fits completely without needing LFN entries, these flags preserve case without consuming extra directory slots.

---

## 4. Multi-Step Implementation Plan

- **Step 1: Audit + Name Limits [COMPLETED]**:
  - Updated `src/vfs.h`, `libmyos/include/dirent.h`, `src/process.h`.
  - Updated path buffers in `src/vfs.c`, `src/syscall.c`, `userland/sh.c`, `userland/ls.c`.
  - Added `-ENAMETOOLONG` checks for component and total path lengths.
- **Step 2: LFN Read Support [COMPLETED]**:
  - Implemented VFAT LFN entry structures (`fat_lfn_entry_t`), checksum calculation, and UTF-16LE / UTF-8 conversion in `src/fat16.c`.
  - Parsed reverse-ordered sequence entries in `fat16_readdir` and populated canonical long names.
  - Case-insensitive lookup supporting both long filenames and 8.3 numeric tail aliases.
- **Step 3: LFN Write Support [COMPLETED]**:
  - Implemented `fat16_generate_83_alias` with lossy conversion, stripping forbidden chars, and collision resolution (`~1` to `~999999`).
  - Implemented `fat16_alloc_dir_slots` to find contiguous spans of free or deleted (`0xE5`) directory entries in both fixed root and cluster-chained subdirectories.
  - Implemented reverse-sequence LFN slot writer with valid checksums and `0xFFFF` padding.
- **Step 4: Rename, Unlink, Rmdir [COMPLETED]**:
  - Multi-slot erasure in `fat16_unlink` and `fat16_rmdir` writing `0xE5` to all LFN entries and the primary short entry.
  - Implemented full and case-only renames in `fat16_rename` across directory trees.
- **Step 5: Clean Up 8.3 Workarounds [COMPLETED]**:
  - Removed truncated legacy aliases from `tools/build_disk.sh`.
  - Restored natural long filenames (`crash_safety_test.elf`, `screenshot.elf`, etc.).
- **Step 6: Automated Test Suite [COMPLETED]**:
  - Added `userland/lfntest.c` testing read of host-created LFNs, direct open/stat/read, creation of long names (>11 chars with spaces), readdir exact matching, 8.3 alias access (`/MYLONG~1.TXT`), case-only rename, cross-name rename, nested long directories, multi-slot unlink/rmdir cleanup, and PMM frame leak assertions.
  - Integrated `lfntest` into automated CI/boot validation (`tools/test_boot.sh`).

---

## 5. Verification & Test Results

- All userland tests (`lfntest`, `fstest`, `concur_test`, `memtest`, `sigtest`, `jobtest`, `fbtest`, `ttytest`) pass cleanly.
- Zero memory / PMM frame leaks detected across repeated LFN operations and directory modifications.
- Host interoperability verified using standard Linux FAT utilities (`mkfs.fat`, `mdir`, `mcopy`, `fsck.fat`).


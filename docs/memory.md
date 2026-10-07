# MyOS Memory Management Architecture (Stage 15)

## 1. Overview and Objectives
Stage 15 overhauls the memory subsystem in MyOS from an early prototype with physical identity mapping to a standard 32-bit x86 Unix-like virtual memory architecture:
- **Higher-Half Kernel**: Kernel VMA linked at `0xC0100000` (physical LMA 1MB) with the entire upper 1GB (`0xC0000000 - 0xFFFFFFFF`) reserved for the kernel.
- **Physical Direct Map**: Physical RAM up to 768MB mapped into higher-half kernel space at `0xC0000000 + phys` using 4MB PSE pages, providing simple, constant-time `P2V(phys)` and `V2P(virt)` translation for kernel access to arbitrary physical pages without temporary mapping gymnastics.
- **Full PMM & Refcounting**: Physical frame allocator scans the entire Multiboot2 memory map (utilizing 256MB+ of RAM), with a per-frame 16-bit reference count array (`pmm_ref`, `pmm_unref`, `pmm_refcount`) to support Copy-On-Write sharing and proper frame reclamation.
- **Robust User Address Space**: User address space occupies `0x00000000 - 0xBFFFFFFF` (3GB). Page 0 (`0x00000000 - 0x00000FFF`) is unmapped so NULL pointer dereferences trap immediately. User executables link at standard Linux x86 base `0x08048000`. User stack sits right below `0xC0000000` (`0xBFFFF000`).
- **VMAs & Demand Paging**: Per-process virtual memory area (VMA) structures represent memory segments (`ANON`, `FILE`, `STACK`, `HEAP`). Demand paging lazily populates pages on first touch.
- **Copy-on-Write (COW) Fork**: `fork()` marks user pages read-only with a software COW bit and increments frame refcounts, eliminating eager page copies and enabling fast process creation.
- **mmap/munmap**: Anonymous and file-backed memory mappings, alongside a modernized userspace heap allocator.
- **Kernel Heap Hardening**: Dynamic growth from the direct map, boundary canaries, double-free detection, and boot self-tests.

---

## 2. Current Memory Layout (Stages 1–14)

### 2.1 Virtual Address Space Map (Current)
In Stages 1–14, paging was initialized with a simple identity map covering the first 16MB of physical RAM:
```
Virtual Address Range           Attributes      Description
-------------------------------------------------------------------------------------
0x00000000 - 0x00000FFF (4KB)   Supervisor-only Identity-mapped page 0 (NULL not trapped!)
0x00001000 - 0x0009FFFF (~640K) Supervisor-only Real-mode / BIOS data & low memory
0x000A0000 - 0x000BFFFF (128KB) Supervisor-only VGA video memory (0xB8000 text buffer)
0x00100000 - 0x00400000 (3MB)   Supervisor-only Kernel text, rodata, data, bss, kheap (2MB)
0x00400000 - 0x007FFFFF (4MB)   Supervisor-only Frame identity map window for kernel
0x00800000 - 0x008FFFFF (1MB)   User R/W/X      User ELF text/data/bss (user.ld base)
0x00900000 - 0x00A00000 (1MB)   User R/W        User heap (HEAP_BASE, 1MB max growth)
0x00A00000 - 0xBFFFFFFF (~3GB)  Unmapped        Unallocated virtual address space
0xC0000000 - 0xC0003FFF (16KB)  User R/W        User stack (USER_STACK_TOP = 0xC0000000)
0xC0004000 - 0xFFFFFFFF (~1GB)  Unmapped        Unused
```

### 2.2 Physical Memory and PMM (Current)
- Kernel binary linked at physical address 1MB (`0x00100000`).
- Frame allocator (`src/pmm.c`): A static bitmap `frame_bitmap[16384]` covering up to 512MB (`MAX_FRAMES = 131072`).
- Multiboot2 parsing marked all RAM as used and freed available memory regions discovered in the MMAP tag.
- Crucially, the PMM only allocated frames from low memory because only the first 16MB was mapped by `paging.c`. If QEMU had 128MB or 256MB, physical frames above 16MB were effectively unusable by the kernel because the kernel had no page mappings to access them.
- No frame reference counting existed. Frames had binary state (0 = free, 1 = used in bitmap). Every page duplication (e.g. `fork`) eagerly allocated a new frame and copied all 4096 bytes.

### 2.3 Kernel Heap (Current)
- `src/kheap.c`: Backed by a fixed 2MB static buffer `heap_region[2 * 1024 * 1024]` in the kernel's `.bss`.
- Embedded inside the kernel image below 4MB.
- First-fit allocator with adjacent block merging, but without guard canaries or dynamic expansion capabilities.

### 2.4 Code Assuming `phys == virt` (Current Audit)
Because `CR3` was active with identity mappings for `0 - 16MB`, several critical kernel subsystems directly conflated physical addresses with virtual pointers:
1. **`src/vga.c`**:
   `terminal_buffer = (uint16_t*) 0xB8000;` directly dereferenced physical VGA text buffer.
2. **`src/kernel.c`**:
   `check_boot_flag_kshell(uint32_t mb_info_addr)`: `uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;` dereferenced GRUB's physical address directly.
3. **`src/pmm.c`**:
   `uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;` parsed Multiboot tags at physical address.
4. **`src/vmm.c`**:
   - `vmm_create_address_space()`:
     `uint32_t dir_phys = pmm_alloc_frame(); uint32_t* dir = (uint32_t*) dir_phys;`
     Directly cast physical address of page directory to a C pointer.
   - `vmm_map_user_page()`:
     `uint32_t table_phys = pmm_alloc_frame(); table = (uint32_t*) table_phys;`
     Directly cast physical page table frame to a pointer.
     `table = (uint32_t*)(dir[dir_index] & ~0xFFFu);`
     Treated physical page table base stored in PDE as a virtual pointer.
   - `vmm_clone_user_pages()`:
     `uint32_t* table = (uint32_t*)(dir_entry & ~0xFFFu);`
     Treated physical PDE table address as virtual pointer.
     `uint8_t* dest_page = (uint8_t*) new_frame_phys;`
     Wrote page copy directly to physical address.
5. **`src/elf.c`**:
   - `load_segment()`:
     `uint8_t* frame = (uint8_t*) frame_phys;`
     Copied ELF executable bytes directly into physical frame pointer.
   - `elf_load_into()`:
     `uint8_t* page_end = (uint8_t*) top_frame_phys + PAGE_SIZE;`
     Constructed user stack (`argc`, `argv`, strings) directly via physical frame pointer.
6. **`src/syscall.c`**:
   - `validate_user_buffer()` and `validate_user_string()`:
     `uint32_t* tbl = (uint32_t*) (dir[dir_idx] & ~0xFFFu);`
     Walked process page directory by treating PDE physical address as virtual pointer.
   - `SYS_SBRK`:
     `uint8_t* frame = (uint8_t*) frame_phys; for (int i = 0; i < 4096; i++) frame[i] = 0;`
     Zeroed newly allocated user heap page using its physical address as pointer.
7. **`src/paging.c`**:
   - `page_directory` and `page_tables` statically defined in kernel `.bss` and loaded directly into `CR3` as physical addresses.
   - Only PDE 0 was shared with child processes (`dir[0] = paging_get_kernel_dir_entry0()`).

---

## 3. Target Memory Architecture (Stage 15)

### 3.1 Higher-Half Virtual Address Layout
The 4GB 32-bit virtual address space is split into a 3GB User Space and a 1GB Kernel Space at boundary `0xC0000000` (PAGE_OFFSET = 3GB):

```
Virtual Address Range            Size     Use / Mapping Details
=====================================================================================
0x00000000 - 0x00000FFF          4KB      UNMAPPED Guard Page: NULL dereferences trap (#PF)
0x00001000 - 0x08047FFF          ~128MB   Unmapped / low user space
0x08048000 - 0x08XXXXXX          Variable User ELF Text, Rodata, Data, BSS (linked at 0x08048000)
0x08XXXXXX - ...                 Variable User Heap (grows upwards on demand via sbrk / VMAs)
...        - 0xAFFFFFFF          Variable Available for large mmap / shared regions
0xB0000000 - 0xBFEFFFFF          ~255MB   User mmap allocations (allocated top-down)
0xBFF00000 - 0xBFFFFFFF          1MB      User Stack (top at 0xBFFFF000, grows downward)
-------------------------------------------------------------------------------------
0xC0000000 - 0xEFFFFFFF          768MB    Kernel Direct Physical Map (P2V: 0xC0000000 + phys)
                                          Mapped using 4MB PSE pages (CR4.PSE = 1).
                                          - 0xC00B8000: VGA text buffer
                                          - 0xC0100000: Kernel image (.text, .rodata, .data, .bss)
                                          - Direct access to all physical frames, page tables, buffers
0xF0000000 - 0xFFFFFFFF          256MB    Dynamic Kernel Heap / Vmalloc / Device MMIO
```

### 3.2 Page Directory Mapping Model
Every process page directory consists of 1024 Page Directory Entries (PDEs):
- **User PDEs (0 to 767, `0x00000000 - 0xBFFFFFFF`)**:
  - Private per-process.
  - Initialized to 0 (unmapped).
  - Populated on demand via VMAs and page faults.
  - PDE 0 covers `0x00000000 - 0x003FFFFF`; PTE 0 inside it is left marked not present.
- **Kernel PDEs (768 to 1023, `0xC0000000 - 0xFFFFFFFF`)**:
  - Shared across every process page directory and the master kernel page directory.
  - When creating any address space, PDEs 768–1023 are copied verbatim from the master kernel directory.
  - Kernel PDEs 768–959 map physical RAM `0 - 768MB` directly with 4MB PSE pages (`PAGE_PRESENT | PAGE_WRITE | PAGE_PSE`).
  - Kernel pages are marked supervisor-only (`PAGE_USER` bit clear).

### 3.3 Physical-to-Virtual Address Translation
With the 768MB direct map active in the kernel, physical addresses are converted to virtual pointers using simple macros:
```c
#define KERNEL_VIRT_BASE 0xC0000000u
#define DIRECT_MAP_LIMIT 0x30000000u /* 768MB */

#define P2V(phys) ((void*)((uintptr_t)(phys) + KERNEL_VIRT_BASE))
#define V2P(virt) ((uint32_t)((uintptr_t)(virt) - KERNEL_VIRT_BASE))
```
Any physical frame allocated by `pmm_alloc_frame()` (e.g. `frame_phys`) is accessed by kernel code as `P2V(frame_phys)`.

---

## 4. Subsystem Overhaul Details

### 4.1 Step 2: PMM Upgrade
- **Full Memory Map**:
  - Scans all entries in Multiboot2 MMAP tag.
  - Initializes `frame_bitmap` covering all detected physical RAM (or capped at 768MB / 1GB).
  - Explicitly reserves:
    1. Low 1MB (`0x00000000 - 0x000FFFFF`): BIOS, BDA, EBDA, VGA memory.
    2. Kernel physical range: `0x00100000` to `V2P(&kernel_end)`.
    3. Multiboot2 info structure: `mb_info_addr` through `mb_info_addr + total_size`.
- **Reference Counting (`uint16_t* frame_refcount`)**:
  - Allocated in early physical memory immediately after `kernel_end`.
  - API:
    - `uint32_t pmm_alloc_frame(void)`: Allocates frame from bitmap, sets `refcount[frame] = 1`, returns physical address.
    - `void pmm_ref(uint32_t phys)`: Increments `refcount[phys / 4096]`.
    - `void pmm_unref(uint32_t phys)`: Decrements `refcount[phys / 4096]`; if it reaches 0, clears bitmap bit and increments `free_frames`.
    - `uint16_t pmm_refcount(uint32_t phys)`: Returns current reference count.
    - `uint32_t pmm_free_count(void)`: Free frame count.
    - `uint32_t pmm_total_count(void)`: Total usable frame count.

### 4.2 Step 3: Higher-Half Kernel Transition (Bootstrapping)
- **Linker Script (`boot/linker.ld`)**:
  - VMA = `0xC0100000`, LMA = `0x00100000` (`AT(0x00100000)`).
  - Kernel symbols live at `0xC0100000+`.
- **Bootstrap in `boot/boot.s`**:
  - Runs in 32-bit protected mode at physical address 1MB.
  - Sets up an early boot page directory at a static physical label:
    - PDE 0 (0-4MB identity map): `0x00000083` (4MB page, PSE, Present, Writable).
    - PDE 768 (`0xC0000000 - 0xC0400000`): `0x00000083` (4MB page mapped to physical 0-4MB).
  - Sets `CR4.PSE` (bit 4 = 0x10).
  - Loads boot page directory into `CR3`.
  - Sets `CR0.PG` (bit 31) and `CR0.WP` (bit 16).
  - Executes long jump / absolute jump to higher-half label `higher_half_start` (`0xC010xxxx`).
  - Sets up stack at higher-half address `stack_top` (`0xC0xxxxxx`).
  - Passes `P2V(mb_info_addr)` to `kernel_main`.
- **Master Direct Map Setup in `kernel_main`**:
  - Creates master kernel directory mapping `0xC0000000 - 0xEFFFFFFF` to physical `0x00000000 - 0x2FFFFFFF` using 4MB PSE pages.
  - Loads master directory into `CR3`.
  - Clears identity mapping (PDE 0 = 0) and flushes TLB via `invlpg` or reloading `CR3`.
  - Page 0 is now completely unmapped.

### 4.3 Step 4: Virtual Memory Areas (VMAs)
- `struct vma`:
  ```c
  typedef enum { VMA_ANON, VMA_FILE, VMA_STACK, VMA_HEAP } vma_type_t;

  typedef struct vma {
      uint32_t start;             /* page-aligned virtual start */
      uint32_t end;               /* page-aligned virtual end (exclusive) */
      uint32_t prot;              /* PROT_READ, PROT_WRITE, PROT_EXEC */
      uint32_t flags;             /* MAP_ANON, MAP_PRIVATE, etc. */
      vma_type_t type;
      struct open_file* file;     /* file for VMA_FILE */
      uint32_t file_offset;
      struct vma* next;
  } vma_t;
  ```
- Stored as a sorted linked list per `process_t`.
- Page Fault Handling:
  - When #PF triggers, handler looks up `faulting_addr` in `current->vmas`.
  - If no VMA contains `faulting_addr`, or if access violates `vma->prot`:
    - If user mode: kills process with signal/exit code 139, logging error details.
    - If kernel mode during `copy_from_user` / `copy_to_user`: gracefully returns `-EFAULT`.

### 4.4 Step 5: Demand Paging & Lazy Allocation
- **Lazy Stack**: Stack VMA initialized from `0xBFF00000` to `0xC0000000`. Pages mapped only on write/read fault.
- **Lazy Heap**: `sys_sbrk` updates `heap_vma->end` without allocating physical frames. On touch, page fault allocates a zeroed frame.
- **Lazy File/BSS**: ELF segments mapped with `vma_type = VMA_FILE` or `VMA_ANON`. On touch, kernel reads file chunk from VFS or zeroes frame.

### 4.5 Step 6: Copy-On-Write (COW) Fork
- `fork()` clones address space without duplicating physical frames:
  - For each present user PTE:
    - Clears `PAGE_WRITE` bit (read-only).
    - Sets bit 9 (`PAGE_COW = 0x200`).
    - Calls `pmm_ref(frame_phys)`.
  - Clones PTE to child with same flags.
  - Flushes TLB in parent.
- Write fault on COW page:
  - If `pmm_refcount(frame_phys) == 1`: restore `PAGE_WRITE`, clear `PAGE_COW`, flush TLB.
  - If `pmm_refcount > 1`: allocate new frame via `pmm_alloc_frame()`, copy 4KB data, call `pmm_unref(old_frame_phys)`, map new frame with `PAGE_WRITE | PAGE_USER`, clear `PAGE_COW`.

### 4.6 Step 7: `mmap` / `munmap`
- Syscall `SYS_MMAP(addr_hint, length, prot, flags, fd, offset)`:
  - Allocates address range top-down below stack (starting at `0xB0000000`).
  - Creates corresponding VMA.
- Syscall `SYS_MUNMAP(addr, length)`:
  - Unmaps pages, unrefs frames, updates/splits VMAs.

### 4.8 Step 9: Tests, Verification, and Memory Test Suite
- Comprehensive userland test suite: `userland/memtest.c` (built as `memtest.elf`).
- Validates:
  1. COW correctness (independent memory space after fork modifications).
  2. COW efficiency (minimal page table allocation on fork, frame count return to baseline).
  3. Demand paging (lazy allocation on touch for 64MB break, shrink restores frames).
  4. Stack auto-growth (2MB recursive descent succeeds, unbounded recursion terminated with exit 139).
  5. `mmap` / `munmap` (anonymous memory, file-backed mapped reading, munmap then touch terminates with exit 139).
  6. Memory protection (NULL dereference, write to code segment, jump to non-executable region, kernel address access all killed cleanly with exit 139 without panicking kernel).
  7. Fork storm (200 sequential forks + 20 concurrent children with zero frame leaks).
- Validated via `tools/test_boot.sh` across QEMU `-m 256` and low-memory `-m 64` configurations.

---

## 5. Syscall Interface (Updated)

| Number | Name | Arguments | Description |
|---|---|---|---|
| 0 | `SYS_EXIT` | `int status` | Terminate calling process with exit code |
| 1 | `SYS_WRITE` | `int fd, const void* buf, size_t len` | Write bytes to file/pipe/console |
| 2 | `SYS_GETPID` | *(none)* | Return calling process PID |
| 3 | `SYS_OPEN` | `const char* path, int flags, int mode` | Open file by path |
| 4 | `SYS_READ` | `int fd, void* buf, size_t len` | Read bytes from file/pipe/console |
| 5 | `SYS_CLOSE` | `int fd` | Close file descriptor |
| 7 | `SYS_FORK` | *(none)* | Copy-On-Write duplicate calling process |
| 8 | `SYS_PIPE` | `int fds[2]` | Create unidirectional IPC pipe |
| 10 | `SYS_EXEC` | `const char* path, const char* argv[]` | Replace process image with ELF executable |
| 11 | `SYS_WAITPID` | `int pid, int* status, int options` | Wait for child process state change |
| 12 | `SYS_SBRK` | `intptr_t increment` | Move heap break (lazy demand-paged up to 256MB) |
| 13 | `SYS_SYNC` | *(none)* | Flush buffer cache to disk |
| 14 | `SYS_LSEEK` | `int fd, int offset, int whence` | Set open file offset |
| 15 | `SYS_STAT` | `const char* path, struct stat* st` | Query file status by path |
| 16 | `SYS_FSTAT` | `int fd, struct stat* st` | Query file status by descriptor |
| 17 | `SYS_GETDENTS` | `int fd, struct dirent* dirp, size_t count` | Read directory entries |
| 18 | `SYS_MKDIR` | `const char* path, int mode` | Create directory |
| 19 | `SYS_RMDIR` | `const char* path` | Remove empty directory |
| 20 | `SYS_UNLINK` | `const char* path` | Remove file link |
| 21 | `SYS_RENAME` | `const char* old, const char* new` | Rename filesystem path |
| 22 | `SYS_CHDIR` | `const char* path` | Change current working directory |
| 23 | `SYS_GETCWD` | `char* buf, size_t size` | Retrieve current working directory |
| 24 | `SYS_DUP` | `int oldfd` | Duplicate file descriptor |
| 25 | `SYS_DUP2` | `int oldfd, int newfd` | Duplicate to specific descriptor |
| 26 | `SYS_FCNTL` | `int fd, int cmd, int arg` | File control operations |
| 27 | `SYS_SLEEP` | `unsigned int seconds` | Sleep process for specified duration |
| 28 | `SYS_TICKS` | *(none)* | Retrieve PIT timer tick count |
| 29 | `SYS_FREE_FRAMES` | *(none)* | Query free physical frame count from PMM |
| 30 | `SYS_MMAP` | `void* addr, size_t len, int prot, int flags, int fd, size_t offset` | Map pages into address space |
| 31 | `SYS_MUNMAP` | `void* addr, size_t len` | Unmap pages from address space |
| 32 | `SYS_MPROTECT`| `void* addr, size_t len, int prot` | Change protection on address range |

---

## 6. Known Limitations
1. **PAE / NX bit**: 32-bit non-PAE paging does not provide a hardware No-Execute bit in page table entries; `PROT_NONE` and unmapped accesses fault in hardware, and instruction fetches on non-executable areas fault on CPUs with NX or on unmapped pages.
2. **Page Swapping / Eviction**: Physical pages are frame-backed or lazy-loaded from files; an on-disk swap partition for paging anonymous memory to disk under heavy memory pressure is not yet implemented.
3. **Direct Map Ceiling**: Direct map covers physical memory up to 768MB (`0xC0000000 - 0xEFFFFFFF`). Machines with >768MB RAM use the first 768MB as logged during PMM initialization.

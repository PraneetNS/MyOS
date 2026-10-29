# MyOS — 32-bit x86 Unix-like Multitasking Operating System

A monolithic, preemptive 32-bit x86 Unix-like operating system written from scratch in C and assembly. MyOS boots via GRUB (Multiboot2), transitions to protected mode, establishes higher-half virtual memory, and provides a POSIX-compatible userspace environment complete with an in-kernel VFS, writable FAT16 filesystem with VFAT Long File Names (LFN), `tmpfs`, UNIX domain sockets, shared memory (`MAP_SHARED`), pseudoterminals (PTYs), signal delivery, an init supervisor (`/sbin/init`), a full Newlib C library toolchain, ported software (Lua 5.4, Kilo editor), a 1024x768 32bpp linear framebuffer with hardware write-combining (PAT), and a client-server graphical windowing system and desktop environment (`wm`, `libwin`, `libui`, `term`, `files`).

---

## Operating System Features

- **Kernel Core & Hardware**:
  - 32-bit x86 protected mode kernel loaded at 1MB and mapped to higher-half (`0xC0000000+`).
  - GDT, IDT, 8259 PIC remapping, 8254 PIT timer at 100 Hz, PS/2 keyboard and IntelliMouse drivers.
  - CMOS RTC driver with UTC epoch tracking and configurable `/etc/timezone`.
  - FPU / SSE support with dynamic CPUID detection and `FXSAVE`/`FXRSTOR` context switching.
  - ACPI shutdown and 8042 keyboard controller reset primitives for clean power management.
- **Memory Management**:
  - Physical Memory Manager (PMM) with frame reference counting over full Multiboot2 memory map.
  - Higher-half virtual memory with 4MB PSE direct physical map (`0xC0000000 + phys`) covering up to 768MB.
  - Sorted per-process Virtual Memory Areas (VMAs) tracking virtual memory segments and protections.
  - Demand paging, lazy heap growth, and automatic stack growth downward up to 8MB.
  - Copy-On-Write (COW) `fork()` with reference-counted page frames and zero leak teardown.
  - Hardened dynamic kernel heap with bidirectional coalescing and magic canary bounds checking.
  - System calls: `mmap`, `munmap`, `mprotect`, `sbrk`.
- **Processes, Scheduling & Signals**:
  - Preemptive round-robin scheduler with per-process dynamic nice values (-20 to 19) and time slices.
  - Non-busy deadline-sorted sleep wait queues (`sleep`, `nanosleep`) with CPU accounting; idle task executes `hlt`.
  - Non-spinning FIFO sleeping mutex subsystem (`kmutex_t`) eliminating timer tick starvation during I/O.
  - Full process hierarchy: orphan reparenting to PID 1 (`/sbin/init`), non-blocking `waitpid()`.
  - System init supervisor (`/sbin/init`) parsing `/etc/init.conf` with `once`, `wait`, and `respawn` rate-limiting.
  - 32 POSIX signals with userspace delivery trampolines, `sigaction`, `sigprocmask`, and `sigreturn`.
  - Terminal process groups, session IDs, and job control (`SIGINT`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`, `SIGCHLD`, `SIGWINCH`).
- **Filesystems & I/O**:
  - Virtual File System (VFS) abstraction with dynamic mount table (`/proc/mounts`).
  - Writable FAT16 driver with full VFAT Long File Names (LFN) support (read, write, multi-slot directory entry allocation, numeric tail generation, UTF-16 decoding, multi-slot unlink/rmdir/rename).
  - ATA PIO hard disk driver with interrupts enabled during transfers, protected by sleeping mutexes.
  - In-memory 64-entry LRU buffer cache (`bcache`) with write-back flushing.
  - In-memory `tmpfs` mounted at `/dev/shm` and `/run` supporting sparse allocation, `truncate`/`ftruncate`.
  - `devfs` providing `/dev/console`, `/dev/tty`, `/dev/null`, `/dev/zero`, `/dev/serial0`, `/dev/fb0`, `/dev/input/*`, `/dev/ptmx`, `/dev/pts/*`.
  - `procfs` virtual filesystem exposing `/proc/meminfo`, `/proc/uptime`, `/proc/cpuinfo`, `/proc/version`, `/proc/cmdline`, `/proc/mounts`, `/proc/net/unix`, `/proc/<pid>/*`.
- **Inter-Process Communication (IPC)**:
  - Anonymous VFS pipes with reader/writer wait queues and EOF signaling.
  - Shared Memory (`MAP_SHARED`): anonymous shared mappings and tmpfs file-backed shared memory with beyond-EOF `SIGBUS` enforcement, `shm_open`, and `shm_unlink`.
  - UNIX Domain Sockets (`AF_UNIX`, `SOCK_STREAM`): bidirectional 64KB kernel ring buffers, tmpfs filesystem binding (`S_IFSOCK`), `socketpair`, `bind`, `listen`, `accept`, `connect`.
  - UNIX98 Pseudoterminals (PTY): master multiplexer `/dev/ptmx`, dynamic slave nodes `/dev/pts/<N>`, line discipline, window resize notification (`TIOCSWINSZ`/`SIGWINCH`), `openpty`, and `forkpty`.
- **Toolchain & Userspace Software**:
  - GCC 13.2.0 + GNU Binutils cross-toolchain targeting `i686-elf` with Newlib C library 4.4.0 (`libc.a`, `libm.a`).
  - System call emulation and POSIX compatibility layer (`libmyos.a`).
  - Full Lua 5.4.6 runtime and bytecode compiler (`lua.elf`, `luac.elf`).
  - Kilo text editor (`edit.elf`) with terminal raw mode and syntax highlighting.
  - Comprehensive POSIX CLI userland: interactive shell (`sh`) with pipelines, redirections, and job control, plus utilities (`cat`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `grep`, `head`, `tail`, `wc`, `diff`, `nl`, `seq`, `cal`, `hexdump`, `sort`, `uniq`, `tee`, `uname`, `which`, `clear`, `env`, `ps`, `top`, `free`, `uptime`, `date`, `time`, `sleep`, `kill`, `poweroff`, `reboot`, `halt`, `shutdown`).
- **Graphics & Window System**:
  - 1024x768 32bpp linear framebuffer with Page Attribute Table (PAT) write-combining MSR acceleration.
  - Fast graphical text console with Terminus 8x16 PSF2 font and dirty row shadow buffering.
  - 2D Graphics Engine (`libgfx`): primitives, anti-aliased gradients, alpha blending, TrueType font rendering (`stb_truetype.h`), and BMP/PNG/JPEG decoding (`stb_image.h`).
  - Compositing Display Server (`wm`): AF_UNIX socket protocol (`/run/display.sock`), out-of-band shared memory surface buffers (`/dev/shm/win-*`), dirty rectangle composition, server-side titlebars, buttons, dragging, resizing, focus, and Alt+Tab overlay.
  - Window Client Library (`libwin`): window lifecycle, double-buffered rendering surface, damage reporting, atomic commits, and event dispatching.
  - GUI Widget Toolkit (`libui`): retained widget hierarchy, labels, buttons, checkboxes, radio groups, progress bars, textboxes, multiline textareas, scrollbars, listboxes, tab controls, menubars, and modal dialogs.
  - Desktop Applications: Graphical Terminal Emulator (`term`), Graphical File Manager (`files`), Widget Demo (`uidemo`), Drawing Program (`paint`), Image Viewer (`view`), Screen Capture (`screenshot`), and Window Control CLI (`wmctl`).
- **Automated Verification**:
  - 15 end-to-end automated boot sessions validating zero frame leaks, memory integrity, stress concurrency, filesystem consistency (`fsck.fat`), and QEMU monitor screendump pixel correctness.

---

## Repository Layout

```text
boot/
  boot.s            -- Multiboot2 header, higher-half bootstrap page directory, and _start
  linker.ld         -- Linker script placing kernel at 1MB physical (0xC0100000 virtual)
  grub.cfg          -- GRUB2 configuration with text, serial, and desktop boot targets
  gdt_flush.s       -- GDT reload assembly helper
  idt_flush.s       -- IDT reload assembly helper
  isr.s             -- 32 CPU exception stubs, 16 IRQ stubs, int 0x80 syscall gate
  paging_asm.s      -- Page directory load (CR3) and paging enable (CR0.PG)
  task_switch.s     -- Context switch stack swap
  usermode_asm.s    -- Ring 3 iret trampoline
  uaccess.s         -- Safe copy_to_user / copy_from_user with page fault recovery
src/
  kernel.c          -- Subsystem initialization, multiboot parsing, and reboot handler
  pmm.c/.h          -- Physical Memory Manager with per-frame reference counts
  paging.c/.h       -- Page directory/table manipulation, demand paging, and COW
  vmm.c/.h          -- Virtual memory manager, address space cloning, and tear-down
  vma.c/.h          -- Virtual Memory Area tracking per-process
  kheap.c/.h        -- Dynamic kernel heap allocator with guard canaries
  process.c/.h      -- Process creation, fork, execve, exit, and orphan reparenting
  scheduler.c/.h    -- Preemptive round-robin scheduler with nice values and wait queues
  kmutex.c/.h       -- Non-spinning FIFO sleeping mutexes
  syscall.c/.h      -- System call table and dispatcher (48+ syscalls)
  signal.c/.h       -- POSIX signal infrastructure and trampoline
  vfs.c/.h          -- Virtual File System mount and vnode abstraction
  fat16.c/.h        -- Writable FAT16 filesystem with VFAT LFN read/write
  bcache.c/.h       -- 64-entry LRU sector buffer cache with writeback
  ata.c/.h          -- Polling/interrupt ATA PIO hard disk driver
  tmpfs.c/.h        -- In-memory filesystem for /dev/shm and /run
  devfs.c/.h        -- Device filesystem (/dev)
  procfs.c/.h       -- Process and system metrics virtual filesystem (/proc)
  pipe.c/.h         -- Unnamed VFS pipes
  socket.c/.h       -- UNIX domain sockets (AF_UNIX SOCK_STREAM)
  pty.c/.h          -- UNIX98 pseudoterminals (/dev/ptmx, /dev/pts/*)
  tty.c/.h          -- Terminal line discipline and canonical input editing
  fb.c/.h           -- Linear framebuffer driver (/dev/fb0)
  ioremap.c/.h      -- Physical I/O memory mapping and PAT write-combining MSR
  console.c/.h      -- PSF2 graphical text console with shadow buffering
  font.c/.h         -- Embedded Terminus 8x16 PSF2 bitmap font
  mouse.c/.h        -- PS/2 IntelliMouse driver with wheel stream
  input.c/.h        -- Linux-compatible input event ring buffers (/dev/input/*)
  rtc.c/.h          -- CMOS RTC real-time clock driver
  fpu.c/.h          -- FPU and SSE context switching
libmyos/            -- Newlib C library syscall stubs and POSIX emulation layer
userland/
  libgfx/           -- 2D graphics, TrueType fonts, and image loading
  libwin/           -- Client-side windowing library and IPC protocol
  libui/            -- Retained GUI widget toolkit
  lua/              -- Ported Lua 5.4.6 runtime source
  wm.c              -- Display server and compositing window manager
  wmctl.c           -- Window management CLI tool
  term.c            -- PTY-backed VT100/xterm-256color graphical terminal emulator
  files.c           -- Graphical file manager with LFN support
  uidemo.c          -- libui widget showcase application
  paint.c           -- 2D painting and raster graphics editor
  view.c            -- Multi-format image viewer (BMP, PNG, JPEG)
  sh.c              -- Interactive shell with job control, pipes, and redirection
  init.c            -- /sbin/init supervisor parsing /etc/init.conf
  kilo.c            -- Kilo text editor (also built as edit.elf)
  *.c               -- POSIX utilities (cat, ls, cp, grep, ps, top, etc.) and test suites
tools/
  build_disk.sh     -- Disk image builder (FAT16 format, userland ELFs, configs)
  test_boot.sh      -- Comprehensive automated regression harness (Sessions 1-15)
  test_step*.py     -- Automated test suites for window system and GUI components
  toolchain/        -- Pinned GNU Binutils, GCC, and Newlib build scripts
Makefile            -- Kernel compilation, ISO creation, and QEMU run rules
```

---

## Build & Run

### Prerequisites (Ubuntu / Debian)

```bash
sudo apt install build-essential gcc-multilib binutils grub-pc-bin grub-common \
                 xorriso qemu-system-x86 mtools dosfstools python3
```

### Building Kernel & Disk Image

```bash
make            # Compiles kernel and builds bootable ISO (myos.iso)
make disk.img   # Builds 64MB FAT16 hard disk image with userland binaries and configs
```

### Running in QEMU

**Text Mode (Default Shell on Console):**
```bash
make run
# or run with full display and disk:
qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d
```

**Graphical Desktop Mode:**
```bash
# Boot and select "MyOS (desktop)" from GRUB menu, or pass mode=desktop:
qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d
```

**Serial Shell Session:**
```bash
qemu-system-i386 -vga std -m 256 -hda disk.img -cdrom myos.iso -boot d -serial stdio
```

### Running Automated Verification Tests

```bash
# Run complete end-to-end regression harness (Sessions 1 through 15):
bash tools/test_boot.sh

# Run window system and GUI component verification suites:
python3 tools/test_step1.py    # libwin & wm protocol
python3 tools/test_step2.py    # Window decorations & wmctl
python3 tools/test_step3.py    # libui widgets & uidemo
python3 tools/test_step4.py    # Graphical terminal emulator (term)
python3 tools/test_step5a.py   # Graphical file manager (files)
```

---

## Progressive Architecture & Development History

The following sections document the step-by-step evolution of MyOS from an initial bare-metal bootloader to a modern multitasking operating system with a graphical windowing system.

---

## Stage 1 (done): Multiboot2 foundation & VGA driver

A minimal x86 kernel that boots via GRUB (Multiboot2), enters 32-bit
protected mode, and prints to the screen via a hand-written VGA text-mode
driver. This is the foundation stage of a "build your own OS" project.

### What's actually happening on boot

1. GRUB reads the Multiboot2 header in `boot.s`, loads `myos.bin` at 1MB,
   switches the CPU to 32-bit protected mode, and jumps to `_start`.
2. `_start` sets up a stack (GRUB doesn't give you one) and calls `kernel_main`.
3. `kernel_main` calls into the VGA driver, which pokes text-mode video
   memory directly at physical address `0xB8000`.
4. The kernel halts the CPU in a loop (`hlt`) since there's nothing else
   to do yet — no interrupts are enabled, so it'll sit there forever.

## Stage 2 (done): GDT, IDT, PIC, timer, keyboard

- `src/gdt.c` + `boot/gdt_flush.s` — our own flat GDT (5 entries: null,
  kernel code, kernel data, user code, user data — the last two unused
  until Stage 4's userspace/ring 3 work).
- `src/idt.c` + `boot/isr.s` + `boot/idt_flush.s` — 32 CPU exception
  vectors + 16 remapped hardware IRQ vectors, each with its own stub
  (x86 gives no other way to know which interrupt fired). Unhandled
  exceptions now print a message and halt instead of silently
  triple-faulting.
- `src/timer.c` — programs the 8253/8254 PIT to fire IRQ0 at 100Hz.
  Not doing anything with the ticks yet, but this is the heartbeat
  a future preemptive scheduler will hook into.
- `src/keyboard.c` — IRQ1 handler, PS/2 scancode set 1 → ASCII,
  prints typed characters directly. (Shift/caps-lock handling and a
  proper input buffer are Stage 3 polish, not blockers.)

Verified: boots in QEMU, all four "[ok]" lines print, `sti` doesn't
crash, and typed keys echo to the screen — confirming the full pipeline
(PIC → IDT → ISR stub → C handler → VGA) actually works end to end.

## Stage 3 (done): paging and a kernel heap

- `src/multiboot2.h` — parses the Multiboot2 memory map tag GRUB passes in
  (previously ignored; `mb_info_addr` is now actually used).
- `src/pmm.c` — physical memory manager: a bitmap over every 4KB frame,
  seeded from the real memory map (verified: found ~125MB free on a
  128MB QEMU VM, matching expectations exactly).
- `src/paging.c` + `boot/paging_asm.s` — builds a page directory + 4 page
  tables identity-mapping the first 16MB, loads `CR3`, sets `CR0`'s PG
  bit. Vector 14 (#PF) now has a real handler that reads `CR2` and
  prints the faulting address instead of triple-faulting.
- `src/kheap.c` — a first-fit free-list `kmalloc`/`kfree` over a static
  2MB backing region. Splits blocks on allocation, merges adjacent free
  blocks on free.

Verified: zero triple faults with interrupt logging enabled, pmm found
the correct free frame count from the real memory map, the heap
smoke-test (alloc 3 blocks → free the middle → alloc a 4th → confirm it
reused the freed space) passes, and keyboard input still works
correctly with the paging unit live (rules out a whole class of subtle
"stack page not mapped" bugs).

## Stage 4 (done): ring 3, syscalls, and a preemptive scheduler

- `src/tss.c` — installs a Task State Segment (GDT slot 5) so the CPU
  knows which kernel stack to switch to on a ring3→ring0 transition.
- `src/paging.c` — identity-mapped pages now carry the `PAGE_USER` bit
  (documented in-file as a teaching-stage simplification: a real kernel
  would only mark a given process's own pages user-accessible, not the
  whole map).
- `src/usermode.h` + `boot/usermode_asm.s` — `enter_usermode()` builds a
  fake interrupt-return frame and `iret`s into ring 3.
- `src/usermode_demo.c` — a program that runs at CPL=3 and can *only*
  reach the kernel via `int 0x80` — it has no direct access to
  `terminal_writestring` or any other kernel function.
- `src/syscall.c` + `isr128` in `boot/isr.s` — the `int 0x80` gate
  (DPL=3, so ring-3 code is allowed to invoke it) with two syscalls:
  `SYS_WRITE` and `SYS_EXIT`.
- `src/task.c` + `boot/task_switch.s` — a round-robin scheduler over
  kernel-mode tasks. `switch_task` is the classic "swap stack pointers,
  let the C call chain encode the continuation" technique: each task's
  suspended state is just wherever its own call stack was sitting when
  the timer interrupted it.
- `src/demo_tasks.c` — two tasks proving genuine preemption: each keeps
  its own independent counter that survives being suspended mid-loop
  and resumed later, interleaved with the other task's output.

Flow: `kernel_main` sets up TSS/syscalls, registers two demo tasks,
then calls `enter_usermode()` to run the ring-3 demo. That program's
`sys_exit` syscall hands off permanently to `scheduler_start()`, which
never returns — from that point on, control only re-enters kernel code
via the timer IRQ's calls to `scheduler_tick()`.

Verified: interrupt-level logging shows exactly 4 syscalls (3 writes +
1 exit, matching the demo program exactly) and hundreds of clean timer
ticks with zero GPFs and zero triple faults. A screenshot confirms real
preemption, not just two tasks racing: Task B is cut off mid-count,
Task A runs for a while, and when Task B resumes it continues from
its own last count rather than restarting — proof each task's context
is genuinely being saved and restored independently.

One incidental bug this stage surfaced and fixed: `terminal_writestring`
had shared mutable cursor state with no protection against a task being
preempted mid-write — visible as garbled interleaved characters during
debugging. Fixed by making each string write a short interrupts-disabled
critical section (`src/vga.c`). Worth remembering as a general pattern:
*any* kernel data touched from more than one task or from an interrupt
handler needs this kind of protection, not just VGA.

## Stage 5 (done): a real filesystem and ELF loading

- `src/ata.c` — polling-mode ATA PIO driver (primary bus, master drive,
  LBA28). No IRQ handling needed for polled I/O, though the controller
  still fires IRQ14 on its own -- safely ignored since nothing's
  registered for that vector (confirmed harmless via interrupt logging).
- `src/fs.c` + `src/fs.h` — a deliberately simple custom filesystem:
  a superblock at LBA 0, a flat directory table (name + start LBA +
  size, 64 bytes/entry) starting at LBA 1, and contiguous file data
  from LBA 16 onward. Read-only, no subdirectories, no allocation --
  real simplicity on purpose, to keep the concept (superblock →
  directory → data) clear before ever reaching for something like FAT.
- `tools/build_disk.py` — host-side Python tool that writes `disk.img`
  in that exact format. This is the "mkfs" for MyFS.
- `userland/hello.c` + `userland/user.ld` + `userland/build.sh` — a
  **completely separate build**: a real standalone ELF32 program,
  compiled and linked on its own (entry `_start`, loaded at a fixed
  `0x800000`), that only ever talks to the kernel via `int 0x80` --
  identical syscall convention to Stage 4's baked-in demo, except this
  binary now lives as genuine bytes on disk.img rather than being
  compiled into the kernel image.
- `src/elf.c` — parses the ELF32 header and program headers, validates
  magic/class/machine/type, copies each `PT_LOAD` segment to its
  `p_vaddr` (documented in-file as relying on the current identity-map
  simplification -- a real loader would map fresh frames into a new
  page directory instead), zeroes `.bss`, and calls `enter_usermode()`
  at `e_entry`.
- `src/shell.c` + keyboard line-buffering (`keyboard_read_line()` in
  `src/keyboard.c`) — an interactive shell: `ls`, `cat <file>`,
  `run <file>`, `help`. Backspace works properly now too (`src/vga.c`
  gained real `\b` handling as part of this).

Flow: `kernel_main` sets up through Stage 4, mounts the filesystem,
then calls `shell_run()`, which never returns under normal operation.
`run <file>`'s ELF launch, like Stage 4's demo, hands off via
`sys_exit` -- except now `sys_exit` calls `shell_run()` again rather
than starting the old scheduler demo, so the user lands back at a
working prompt after their program exits.

**Verified end-to-end, not just visually:** booted with `-hda disk.img`
attached, ran `ls` (lists both files correctly), `cat hello.txt` (full
file content read via ATA and printed correctly), and `run hello.elf`
(loader printed the correct parsed entry point `0x00800000` matching
the linker script exactly, loaded segments, entered ring 3, the program
printed its 4 messages via real syscalls, exited cleanly, and the shell
re-prompted). Interrupt-level logging over the whole session shows
exactly 428 timer ticks, 55 keyboard IRQs matching keystrokes typed,
5 syscalls matching `hello.c`'s code exactly (4 writes + 1 exit), and
zero unexpected vectors, zero GPFs, zero triple faults.

**Known limitations, honestly documented rather than hidden:**
- Every process still shares the kernel's one page directory (the
  Stage 4 "identity-map everything as user-accessible" simplification
  persists). No memory isolation between processes yet.
- `sys_exit` re-entering `shell_run()` via a fresh nested C call (rather
  than a true return, for the same reasons explained in Stage 4) means
  kernel stack usage grows slightly with every `run` command in a
  session. Fine for a demo; a real kernel would tear down the process
  and return to a scheduler loop instead, as Stage 4's original demo did.
- The old Stage 4 baked-in ring-3 demo and 2-task scheduler demo
  (`src/usermode_demo.c`, `src/task.c`, `src/demo_tasks.c`) are still in
  the tree and still compile, just no longer called from `kernel_main`
  by default -- kept as reference/available to re-enable.

## Building and running Stage 5 (updated)

```bash
make                       # kernel + myos.iso, as before
./userland/build.sh        # builds userland/hello.elf
python3 tools/build_disk.py  # builds disk.img from hello.txt + hello.elf

qemu-system-i386 -hda disk.img -cdrom myos.iso -boot d
```

Note the new `-hda disk.img`: the kernel now expects a second drive
attached at the primary ATA bus (ports 0x1F0-0x1F7) for its filesystem,
separate from the GRUB boot CD.

## Stage 6 (done): real process isolation

- `src/paging.c` — reworked so only directory entry 0 (0-4MB, where the
  kernel image + heap live -- confirmed at build time to stay under 4MB)
  is identity-mapped and, critically, **supervisor-only** now: user-mode
  code can no longer touch kernel memory at all. Entries 1-3 remain as a
  kernel-only physical-frame identity map the kernel itself uses while
  setting up new processes, not something any process's own page
  directory inherits.
- `src/vmm.c` + `src/vmm.h` — the actual isolation mechanism:
  `vmm_create_address_space()` allocates a fresh page directory (via the
  Stage 3 physical memory manager) that shares only the kernel's
  supervisor-only entry 0; `vmm_map_user_page()` backs a virtual page
  with a **freshly allocated physical frame** on demand. Two processes
  loaded at the identical virtual address now get genuinely different
  physical memory -- this is the difference between Stage 5's shared
  identity map and real virtual memory.
- `src/elf.c` — rewritten to load each `PT_LOAD` segment page-by-page
  into a new address space's fresh frames (instead of writing directly
  into the old shared identity map), and to map a proper per-process
  user stack at the classic `0xC0000000` convention.
- Page fault handling (`src/paging.c`) now decodes the hardware error
  code and **contains** user-mode faults instead of halting the whole
  machine: it prints what happened, restores the kernel's own address
  space, and hands control back to the shell. A kernel-mode fault (a
  real kernel bug) still halts, since that's genuinely fatal.
- `userland/badwrite.c` — a second userland program built specifically
  to prove protection works: it deliberately writes to kernel memory at
  `0x100000` and expects to be killed for it.

**A real bug this stage caught, worth remembering:** the first working
build of this stage silently broke the keyboard after any program's
first exit. Cause: `sys_exit` (and the new page-fault recovery path)
call `shell_run()` directly rather than returning through the syscall
handler's normal assembly epilogue -- and that epilogue is where `sti`
re-enables interrupts. Skipping it left interrupts permanently disabled
after the first `run` command in any session. Fixed with an explicit
`sti` at both call sites. This is the same category of bug as Stage 4's
VGA reentrancy issue: shortcuts around the normal interrupt-return path
have to manually redo whatever that path would have done. Confirmed
fixed by running three programs consecutively in one session and
checking the interrupt log shows exactly 5 syscalls x 3 = 15, not 5.

**Verified, with hard evidence, not just visual inspection:**
- Two consecutive `run hello.elf` calls produced page directories at
  `0x0021D000` and `0x00225000` -- different physical memory every time,
  proving real per-launch isolation, not address reuse.
- `run badwrite.elf` triggered `*** PAGE FAULT at 0x00100000
  (err=0x00000007, user-mode, write) ***` -- the exact, correctly
  decoded hardware fault for a user-mode write to a present-but-
  protected page. The process was terminated; the shell recovered
  cleanly; `run hello.elf` immediately after succeeded normally,
  proving the whole OS survived a memory protection violation instead
  of crashing.
- Zero triple faults and zero unexpected interrupt vectors across every
  test, confirmed via QEMU's `-d int` interrupt-level logging, not just
  screenshots.

## Building and running Stage 6 (updated)

```bash
make                        # kernel + myos.iso
./userland/build.sh          # now builds every userland/*.c, including badwrite.elf
python3 tools/build_disk.py  # disk.img now has 3 files

qemu-system-i386 -hda disk.img -cdrom myos.iso -boot d
```

Try `run badwrite.elf` yourself once booted -- it's the clearest way to
see Stage 6's actual payoff.

## Stage 7 (done): unifying the scheduler with real processes

- `src/process.h` + `src/process.c` — a real process control block:
  address space, saved kernel-mode `esp`, a dedicated kernel stack (for
  `TSS.esp0`), entry point, user stack top, and state
  (`READY`/`EXITED`/`UNUSED`). The shell itself is process 0: always
  resident, runs in ring0 in the kernel's own address space, entry point
  is `shell_run` directly (no ring3 trampoline needed for it).
- `src/scheduler.c` + `src/scheduler.h` — round-robin over the process
  table, reusing Stage 4's exact `switch_task` mechanism (the same
  "swap stack pointers, let the C call chain encode the continuation"
  trick), but now correctly handling per-process `CR3` and `TSS.esp0` on
  every switch, which Stage 4 never needed since its kernel threads all
  shared one address space.
- `src/elf.c` — split from Stage 5/6's "load and immediately
  `enter_usermode`" into `elf_load_into()`, which only loads a process's
  segments into a *given* address space and hands back its entry point.
  Deciding *when* it actually runs is now the scheduler's job, not the
  loader's.
- `src/vmm.c` — every address space now tracks every physical frame it
  owns (directory, page tables, data pages) so `vmm_destroy_address_space()`
  can give them all back to the physical memory manager on process exit,
  instead of leaking them for the rest of the session as Stage 6 did.
- `src/shell.c`'s `run <file>` is no longer blocking: `process_spawn_from_elf()`
  loads the ELF into a brand new process slot and returns immediately --
  the shell prints its next prompt right away while the new process runs
  **concurrently**, preemptively time-sliced against the shell and any
  other running processes, exactly like Stage 4's two kernel threads did,
  except these are now real, isolated, ring-3 user processes.

**Verified with hard evidence of genuine concurrency, not just
sequential dressed up to look concurrent:** launching `hello.elf`
showed the shell's *own next prompt* land interleaved with the
process's output (`myos> Hello from a REAL ELF binary...`), and a
follow-up `help` command was typed and answered while the process was
still active -- proof the shell moved on immediately rather than
blocking. Launching two processes back-to-back showed keystroke-level
interleaving (`myos> rHello from...` -- the `r` of a second `run`
command landing inside the first process's still-running output).
`badwrite.elf` was re-verified against the new teardown path: exactly
one `v=0e` (page fault) in the interrupt log, process terminated
cleanly via `scheduler_exit_current()`, zero triple faults across every
test.

**A design improvement that eliminated a whole bug class, not just
patched it:** Stage 5/6's `sys_exit` called `shell_run()` as a bare
function call, which skipped the interrupt handler's normal `sti`
epilogue and silently disabled interrupts forever after any program's
first exit (worked around with a manual `sti`, documented in Stage 6's
notes). Routing exit through `scheduler_exit_current()` -> `switch_task`
instead means every process transition now goes through `switch_task`'s
own `popf`, which correctly restores/sets the interrupt flag for
whatever gets resumed -- the same mechanism Stage 4 relied on
originally. No manual `sti` needed anywhere in Stage 7's exit or
page-fault paths. Worth remembering: the *design* fix (route everything
through one correct mechanism) was better than the *patch* fix
(remember to add `sti` at every call site) -- the patch only works
until the next new call site forgets it.

**Known limitations, honestly documented:**
- Round-robin only, fixed `MAX_PROCESSES=4` slots (shell + 3), no
  priorities.
- No `fork`/`exec` -- `run` always creates a brand new process from a
  fresh ELF; there's no way to spawn a *child* of an existing process.
- No inter-process communication, no `wait()`, no process hierarchy.
- The kernel stack size (8KB per process) and owned-frame tracking
  table (64 frames per address space) are fixed, generous-for-this-demo
  constants, not dynamically sized.

## Stage 8 (done): file I/O syscalls, process hierarchy, and a userland libc

- `src/process.h`/`.c` — every process now has a real `pid` and `ppid`
  (the shell is pid 0; `next_pid` counts up from 1), plus a per-process
  file descriptor table (`MAX_FDS=4`) backed directly by `fs.c`.
- `src/fs.c` gained `fs_read_range()` — partial, offset-based reads (the
  building block `SYS_READ` needed; `fs_read_file()` from Stage 5 still
  exists for whole-file reads like the shell's `cat`).
- `src/syscall.c` grew four new syscalls: `SYS_GETPID`, `SYS_OPEN`,
  `SYS_READ`, `SYS_CLOSE` (straightforward — they just validate and
  delegate to the calling process's fd table + `fs.c`), and
  `SYS_SPAWN_WAIT` (spawns a child process and **blocks the caller**
  until it exits -- the missing synchronous counterpart to the shell's
  always-concurrent `run`).
- `src/scheduler.c` gained a new process state (`PROC_WAITING`) and
  `scheduler_wait_for()` / `wake_waiters_for()`: a waiting process is
  excluded from the round-robin rotation (same mechanism that already
  excluded `EXITED`/`UNUSED` processes) until its child calls
  `scheduler_exit_current()`, which wakes it back to `READY`. The
  blocked process's eventual return value was already written into its
  saved register frame *before* it blocked, so waking it up looks, from
  its own code's perspective, exactly like an ordinary function return.
- `userland/libc.h` — a small shared header of `static inline` syscall
  wrappers (`sys_write`, `sys_exit`, `sys_getpid`, `sys_open`,
  `sys_read`, `sys_close`, `sys_spawn_wait`, plus a `print_uint` helper),
  so new userland programs don't hand-roll `int 0x80` each time.
  `hello.c` and `badwrite.c` were refactored to use it.
- `userland/reader.c` — proves file I/O syscalls work: opens
  `hello.txt`, reads it in 32-byte chunks via `sys_read`, all from
  userland, with no help from the shell's kernel-side `cat`.
- `userland/parent.c` — proves `sys_spawn_wait` works: spawns
  `hello.elf` as a genuine child process and blocks until it exits,
  contrasting directly with the shell's fire-and-forget `run`.

**A real, subtle bug this stage caught -- a compiler bug, not a kernel
bug, and worth remembering for exactly that reason:** the first version
of `reader.elf` printed blank space everywhere a number should have
appeared (`pid` and the byte count). Disassembling the `-O2` build
showed `print_uint()`'s loop correctly computing each digit's value via
the usual multiply-by-magic-constant division trick, but **never
actually storing the resulting ASCII byte into the output buffer** --
confirmed by comparing against an `-O0` build of the same source, where
the `add $0x30,%eax` / `mov %cl,(%eax)` store is clearly present.
`-O1` had the same problem; `-O0` doesn't. Fixed by building userland
at `-O0` (documented in `userland/build.sh` -- these are small demo
programs, so the lost optimization costs nothing). The lesson: a
freestanding, no-libc environment is exactly the kind of place where an
optimizer's hosted-environment assumptions can produce a silent,
plausible-looking wrong answer instead of a crash -- worth treating
optimization level as a variable to test, not a given, whenever
something's output looks subtly incomplete rather than obviously broken.

**Verified with exact, hand-checked evidence:** `reader.elf` reported
`pid 1` (correct -- first process spawned after the shell) and `393
bytes read via syscalls` (exactly matching `hello.txt`'s real size).
`parent.elf` (pid 1) spawned `hello.elf` as pid 2, blocked, and resumed
only after the child's `[ok] process exited` printed, correctly
reporting `child (pid 2) finished`. The shell answered `help` normally
immediately afterward, proving full recovery. The interrupt log showed
**exactly 16 syscalls** for that whole session -- 11 from `parent.elf`
plus 5 from its child `hello.elf`, hand-counted in advance from the
source and matched exactly. Zero triple faults throughout.

## Stage 9 (done): fork() and pipes

- **`fork()`** — the genuinely new, harder mechanism this stage adds.
  `src/vmm.c`'s `vmm_clone_user_pages()` walks the calling process's
  entire page directory and duplicates every mapped page into a fresh
  address space, each backed by an independent physical frame (reusing
  the same "read via the live CR3, write via the frame's physical
  address" trick `elf.c` already used). `boot/resume_state.s`'s
  `resume_saved_state()` is the other half: a full CPU-state restore
  (every general register plus `eip`/`cs`/`eflags`/`useresp`/`ss`) that
  lets a forked child resume at the *exact* instruction the parent was
  at, not just at a fresh entry point like every other process so far.
  `process_fork()` in `src/process.c` ties both together and forces the
  child's saved `eax` to 0 -- the parent (still running the same
  syscall handler) sees the child's pid, achieving classic "one call,
  two returns" Unix semantics.
- **Pipes** — deliberately the simplest thing that demonstrates real
  blocking IPC: `src/pipe.c` is a single global 256-byte ring buffer,
  not a general `pipe()` syscall with fd pairs. `SYS_PIPE_READ` blocks
  the calling process via the exact same scheduler mechanism as Stage
  8's `SYS_SPAWN_WAIT` (a new `PROC_WAITING` sentinel value in
  `scheduler.c`) whenever the pipe is empty, and `SYS_PIPE_WRITE` wakes
  any blocked reader.
- `userland/forktest.c` -- proves `fork()` end to end.
- `userland/producer.c` + `userland/consumer.c` -- proves blocking pipe
  IPC end to end; launch `consumer.elf` first so it blocks immediately,
  then `producer.elf`, and watch the consumer wake as data arrives.

**Verified, with the fork() result being unambiguous:** `forktest.elf`
(pid 1) printed `I am the PARENT, pid 1. fork() gave me child pid 2`,
then a **separate process** (pid 2) resumed at the exact same source
line and printed `I am the CHILD, pid 2` with `result == 0` -- one
`sys_fork()` call, two independent continuations, exactly matching
real Unix semantics. `consumer.elf` printed `waiting for message 1...`
and correctly produced nothing further until `producer.elf` started
writing; data then flowed through correctly. Zero triple faults, zero
unexpected interrupt vectors (checked via `-d int` logging) across
every test.

**Worth understanding, not a bug:** the producer/consumer demo's output
sometimes shows a consumer read pulling back more than one message, or
splitting one message across two reads (e.g. `got: and final message`
instead of the whole third message). This is correct byte-stream pipe
behavior -- `pipe_read()` has no concept of message boundaries, exactly
like a real Unix pipe without an application-level framing protocol
layered on top. It's proof the mechanism is realistic, not proof of a
flaw.

**Known simplifications, honestly scoped:**
- One global pipe, not a general IPC mechanism -- every process shares
  the same buffer. A real `pipe()` would return a private fd pair per
  call and integrate with the existing fd table.
- `pipe_write()` never blocks -- a full buffer just silently drops the
  overflow. A real implementation would block the writer too.
- Forked children inherit a **copy** of open file descriptors (same
  file, independent offset) rather than sharing a position the way real
  `fork()`'s descriptor table does.
- No `exec()` -- there's no way for a forked child to replace itself
  with a different program; it can only continue running a copy of its
  parent's code.

## Stage 10 (done): exec() -- completing fork()+exec()+wait()

- **`SYS_EXEC`** (`src/syscall.c`) -- the missing piece that makes
  `fork()` actually useful. It loads a fresh ELF into a **brand new**
  address space first (so a failure leaves the calling process
  completely untouched -- real `exec()` semantics: it only fails to
  return), then swaps it into the *current* process in place of its old
  one via `vmm_destroy_address_space()` on the old space, and jumps
  straight to the new program's entry point with `enter_usermode()`.
  The process keeps its pid, ppid, kernel stack, and open file
  descriptors -- only its code, data, and entry point change.
- **`SYS_WAIT`** -- a thin, general version of Stage 8's
  `scheduler_wait_for()`: given a pid (rather than always the pid of a
  process you just spawned), block until it exits, or return
  immediately if it already has. This is what actually lets `fork()`
  and `wait()` compose: a process can `fork()`, then separately
  `wait()` on the exact child it got back.
- `userland/forkexec.c` -- the classic Unix process-creation idiom, for
  real: `fork()` to create a copy, `exec()` in the child to become a
  different program, `wait()` in the parent to block until it's done.
  This is exactly how a real shell implements running a command.

**Verified with unambiguous evidence that exec() genuinely replaced the
process's code, not just printed a misleading message:** `forkexec.elf`
(pid 1) forked; the child (pid 2) printed its own message, then called
`sys_exec("hello.elf")`. The very next output was the kernel's ELF
loader firing again with entry `0x00800023` -- different from
`forkexec.elf`'s own entry `0x00800109` -- followed by `hello.elf`'s
exact greeting text, still under **pid 2**. The parent's `sys_wait(2)`
correctly blocked until that transformed process actually exited (not
just until the original fork point), matching real `wait()` semantics
even though the child became a completely different program mid-flight.
Zero triple faults, zero unexpected interrupt vectors.

## Stage 11 (done): a real sbrk() heap syscall

- **`SYS_SBRK`** (`src/syscall.c`) -- classic `sbrk()` semantics: each
  process gets a fixed heap region starting at `HEAP_BASE` (0x900000,
  clear of code and the stack), capped at 1MB of growth
  (`HEAP_MAX`). Growing the heap maps new pages on demand via
  `vmm_map_user_page()` (the same per-process, freshly-allocated-frame
  mechanism every other stage's memory has used) and explicitly zeroes
  each new page -- matching real `brk()`/`mmap()` behavior, where fresh
  memory always reads as zero. Returns the *previous* break, so the
  newly available range is `[return value, return value + increment)`.
- `process.h` gained `heap_end`/`heap_mapped_up_to` per process.
  `fork()` correctly **inherits** the parent's heap bookkeeping (since
  `vmm_clone_user_pages()` already copied any heap pages the parent had
  grown into); `exec()` correctly **resets** it to `HEAP_BASE` (the old
  heap died along with the old address space it belonged to).
- `userland/heaptest.c` -- proves it's real, usable memory, not just a
  number: grows the heap, confirms the first byte reads as zero,
  writes and reads back an actual string, then grows again and checks
  the second region starts exactly one page later.

**Verified with every value hand-checked, not just "it printed
something":** initial break was exactly `0x00900000` (`HEAP_BASE`
precisely); the first byte of freshly grown memory really was `0`
before any write; the string written into the new region read back
correctly; the second `sbrk()` call returned an address exactly
`0x1000` (4096) past the first, matching the page size exactly; a byte
written to the second region read back as `88`, which is exactly the
decimal ASCII value of `'X'`. Zero triple faults.

**A limit worth being explicit about, not just for this stage:** every
frame the kernel needs to *write into directly* (via its physical
address, using the identity-map trick every stage since `elf.c` has
relied on) must come from `pmm_alloc_frame()`'s low, sub-16MB range --
true so far only because the system hasn't allocated enough total
frames to exhaust it. `sbrk()` shares this same latent constraint. It's
been implicitly true since Stage 5 and never actually hit in testing,
but it isn't asserted or guarded anywhere in the code. Worth fixing
properly (a temporary kernel mapping for high frames) before pushing
memory usage much further, rather than continuing to rely on scale not
yet having exposed it.

## Stage 13 (done): VFS + Writable FAT16 Filesystem

Stage 13 introduces a Virtual File System (VFS) abstraction layer, a full write-back buffer cache with ATA PIO writes, a complete read/write FAT16 filesystem driver, POSIX-style file system syscalls with user pointer validation, modular userland utilities (`ls`, `cat`, `echo`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `touch`, `pwd`), a feature-rich userland shell with `/bin` PATH lookup and PID 1 respawn, and automated reboot persistence testing verified by host-side `fsck.fat`.

### 1. Block Layer + ATA Write & Buffer Cache (Step 1)
- **ATA LBA28 PIO Writes (`src/ata.c`, `src/ata.h`)**: Added `ata_write_sector()` utilizing ATA command `0x30` with `rep outsw`, plus ATA cache flush `0xE7` (`ata_flush()`).
- **Buffer Cache (`src/bcache.c`, `src/bcache.h`)**: Implemented a 64-block (32KB) LRU buffer cache for 512-byte disk sectors:
  - `bcache_read(lba, buf)` / `bcache_write(lba, buf)`
  - Write-back policy with dirty flags (`bcache_sync()` flushes all dirty buffers and flushes the drive cache).
  - Explicit synchronization on shutdown and process exit.
- **`SYS_SYNC` (Syscall 13)**: Userspace `sync()` syscall flushing all dirty cache buffers to disk.

### 2. VFS Core Architecture (Step 2)
- **Vnode & Filesystem Operations (`src/vfs.c`, `src/vfs.h`)**:
  - `struct vnode`: Represents filesystem nodes (`VNODE_FILE`, `VNODE_DIR`, `VNODE_CONSOLE`, `VNODE_PIPE`) with reference counting, sizes, and driver operations pointers.
  - `struct fs_ops`: Pluggable filesystem interface: `lookup`, `read`, `write`, `create`, `mkdir`, `unlink`, `rmdir`, `rename`, `readdir`, `truncate`, `stat`.
  - Mount table supporting filesystem mounting at arbitrary paths (root mounted at `/`).
- **Path Resolution**: `vfs_resolve_path()` and `vfs_resolve_parent()` resolve absolute and relative paths, normalizing `.`, `..`, and repeated slashes `/`.
- **Open File Table & Refcounting**:
  - `struct open_file`: Tracks `vnode*`, file offset, access flags (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`), and reference count.
  - Processes share `open_file` entries across `fork()` (`refcount++`), preserve them across `exec()`, and decrement/free them on `close()` and `exit()`.
  - File descriptors 0, 1, 2 map to shared `OPEN_FILE_CONSOLE` entries.
- **Per-Process CWD**: Current working directory stored in each PCB (`process_t->cwd`), inherited on `fork()`.

### 3. FAT16 Filesystem Driver (Step 3)
- **FAT16 Driver (`src/fat16.c`, `src/fat16.h`)**: Full implementation of `fs_ops` for FAT16:
  - BIOS Parameter Block (BPB) and Extended Boot Record parsing.
  - File Allocation Table cluster allocation, chain reading, extending, and freeing.
  - Root directory and hierarchical subdirectory traversal and entry allocation.
  - 8.3 filename parsing and creation (case-insensitive matching, uppercase on-disk storage, skipping LFN entries).
- **Disk Image Generator (`tools/build_disk.sh`)**: Formats 32MB raw image using `mkfs.fat -F 16`, creates `/bin`, and populates binaries and data files using `mcopy`.

### 4. POSIX Syscall Interface & Pointer Validation (Step 4)
- **Syscall Suite**:
  - `open(path, flags, mode)` with `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`
  - `read(fd, buf, len)`, `write(fd, buf, len)`, `close(fd)`
  - `lseek(fd, offset, whence)` with `SEEK_SET`, `SEEK_CUR`, `SEEK_END`
  - `stat(path, st)`, `fstat(fd, st)`
  - `getdents(fd, dirp, count)` for directory enumeration
  - `mkdir(path, mode)`, `rmdir(path)`, `unlink(path)`, `rename(old, new)`
  - `chdir(path)`, `getcwd(buf, size)`, `sync()`
- **Memory Safety & Pointer Validation**:
  - Strict validation of user buffers and strings: verifies addresses lie within user bounds (`[0x400000, 0xC0000000)`), validates page directory and table present/user bits, and verifies write permissions for output buffers.
  - Returns negative errno codes (`-EFAULT`, `-ENOENT`, `-EEXIST`, `-ENOTEMPTY`, `-EBADF`, `-EISDIR`, `-ENOTDIR`, `-EINVAL`, etc.) defined in shared header `src/errno.h`.
- **Userland Wrappers (`userland/libc.h`)**: Provides standard C function wrappers and structures (`struct stat`, `struct dirent`).

### 5. Modular Userland Utilities & Shell (Step 5)
- **Independent Binaries (`userland/*.c`)**:
  - `ls.c`: Lists directory entries; supports `-l` showing directory tags and file sizes.
  - `cat.c`: Concatenates and prints file contents from path arguments or stdin.
  - `echo.c`: Echoes arguments to stdout.
  - `mkdir.c` / `rmdir.c`: Creates and removes directories.
  - `rm.c`: Unlinks files via `unlink()`.
  - `cp.c`: Copies files across paths.
  - `mv.c`: Moves/renames files via `rename()`.
  - `touch.c`: Creates empty files.
  - `pwd.c`: Prints current working directory.
- **Enhanced Shell (`userland/sh.c`)**:
  - Dynamic prompt displaying current working directory (`sh:<cwd>$ `).
  - Builtin `cd` modifying process working directory via `chdir()`.
  - Builtins `cat` and `ls` removed in favor of external binaries.
  - PATH lookup: checks cwd, then `<cmd>.elf`, then `/bin/<cmd>`, and `/bin/<cmd>.elf`.
- **PID 1 Shell Respawn**: If PID 1 (`sh.elf`) exits, the kernel detects it, logs a message, and respawns `sh.elf`, ensuring the system never dies.

### 6. Automated Verification & Test Harness (Step 6)
- **Extended Test Suite (`tools/test_boot.sh`)**:
  - **Session 1 (Commands & Regression Suite)**: Verifies `pwd`, `mkdir /tmp`, `cd /tmp`, `touch`, `cp`, `mv`, `rm`, `ls -l`, multi-cluster read of 4KB+ `/large.txt`, `fstest` (edge cases: `ENOENT`, `EEXIST`, `ENOTEMPTY`, `EFAULT`, EOF, multi-cluster write/read-back, nested directories, and sync), `hello.elf`, `argtest.elf`, `forktest.elf`, `forkexec.elf`, `heaptest.elf`, `badwrite.elf`, and shell exit respawn.
  - **Session 2 (Persistence Across Reboot)**: Boots a second QEMU instance with the *same* `disk.img` without rebuilding and asserts files created in Session 1 (`/persist.txt` and `/nest1/nest2/test.txt`) retain their data.
  - **Session 3 (Host Filesystem Integrity)**: Runs host-side `fsck.fat -n disk.img` to ensure zero filesystem corruption, and cross-checks content via `mdir` and `mtype`.

---

## Stage 14 (done): Pipes, dup/dup2, fcntl, waitpid, shell pipelines, and filters

- **Kernel Pipes as File Descriptors (`SYS_PIPE`, ID 8)**:
  - Replaced the legacy single global pipe with dynamically allocated `pipe_t` objects (`src/pipe.c`, `src/pipe.h`).
  - 4KB ring buffers allocated via `kmalloc`, tracked by `reader_count` and `writer_count`.
  - Blocked readers sleep on `read_wait` channel until data arrives or all writers close; empty pipe with 0 writers returns 0 (EOF).
  - Blocked writers sleep on `write_wait` channel until buffer space opens up; writing with 0 readers returns `-EPIPE`.
  - `pipe(int fds[2])` installs two distinct `open_file_t` entries (`OPEN_FILE_PIPE`, read and write) in the calling process's descriptor table.
  - Pipe lifetimes track `open_file` refcounts, not raw fd table slots: `fork()` and `dup()` increment open_file references without double-counting reader/writer counts.
  - Kernel asserts and serial logging monitor open_file and pipe refcounts against going negative or leaking.
- **`dup()` (ID 24), `dup2()` (ID 25), and `fcntl()` (ID 26)**:
  - `SYS_DUP(fd)` duplicates an open file into the lowest available fd slot.
  - `SYS_DUP2(old, new)` closes `new` if currently open, and aliases `old` onto `new` with `open_file_ref()`. Replacing fds 0, 1, 2 is fully supported.
  - `SYS_FCNTL(fd, cmd, arg)` implements `F_GETFD`, `F_SETFD`, and `F_DUPFD`.
  - Added per-descriptor `FD_CLOEXEC` flag array in PCB and `O_CLOEXEC` in `SYS_OPEN`. Descriptors marked `FD_CLOEXEC` are closed automatically across `SYS_EXEC`.
- **`waitpid()` with Exit Status (ID 11)**:
  - `SYS_WAITPID(pid, status, options)` supports waiting for specific children (`pid > 0`) or any child (`pid == -1`).
  - Processes exit into `PROC_ZOMBIE` state, preserving their exit code in `pcb->exit_code`.
  - Reaping process frees zombie PCB, stack, and address space, storing `(exit_code & 0xff) << 8` into userspace `*status`.
  - Orphaned children are automatically reparented to PID 1.
  - Implemented `WNOHANG` for non-blocking status polling and deleted the legacy `SYS_SPAWN_WAIT`.
- **Shell Redirections, Pipelines, and Quotes (`userland/sh.c`)**:
  - Full redirection support: `< file`, `> file`, `>> file` (append), `2> file`, `2>&1`.
  - Multi-stage pipelines: `a | b | c ...` supporting up to 8 stages with inter-stage pipes, clean child fd substitution via `dup2`, and parent waiting for all stages with `$?` tracking the last command.
  - Background execution (`&`) with non-blocking zombie reaping (`waitpid(-1, &status, WNOHANG)`) before each prompt turn.
  - Robust tokenizer supporting single quotes (`'...'`), double quotes (`"..."`), backslash escapes, and `$?` exit status variable expansion.
  - Command chaining via semicolon `;`.
- **Userland Filter Utilities (`userland/`)**:
  - `wc` (-l, -w, -c), `head` (-n), `tail` (-n), `grep` (fixed string, -v, -n, -i), `sort` (in-memory quicksort), `uniq`, `tee`, `yes` (backpressure testing), `hexdump`, `true`, `false`, `sleep` (`SYS_SLEEP` ID 27, `SYS_TICKS` ID 28), and `diff`.
  - `stress.elf`: Runs 50 iterations of `pipe() + fork() + exec("true") + waitpid()` and verifies physical frame count returns to baseline (`SYS_FREE_FRAMES` ID 29) with zero memory or descriptor leaks.
- **Automated Verification (`tools/test_boot.sh`)**:
  - **Session 1**: Core boot, shell, file utilities, multi-cluster reads, VFS edge-case regression suite.
  - **Session 2**: Persistence across QEMU reboot on the same disk image.
  - **Session 3**: Comprehensive Stage 14 test suite: redirection `>`, `>>`, `<`; pipes with `wc -l`; `yes | head -n 5` EPIPE termination; 4-stage pipeline `ls /bin | grep elf | sort | head -n 3`; `tee` and `diff`; `false; echo $?` and `true; echo $?`; stderr redirection `2>`; >64KB transfer through 4KB ring buffer (`cat /big.txt | wc -c`); and 50-iteration zero-leak stress test.
  - **Session 4**: Host-side `fsck.fat -n disk.img` verification and `mtype` cross-check.

---

## Syscall Reference Table

| Syscall | Vector | ID (`eax`) | Arguments | Return (`eax`) | Description |
|---|---|---|---|---|---|
| `SYS_EXIT` | `0x80` | `0` | `ebx` = status | does not return | Terminates caller, saves exit status, moves to `PROC_ZOMBIE`, and reparents children to PID 1. |
| `SYS_WRITE` | `0x80` | `1` | `ebx` = fd, `ecx` = buf, `edx` = len | bytes written (negative errno on error) | Writes buffer to specified file descriptor (file, console, or pipe). |
| `SYS_GETPID` | `0x80` | `2` | none | PID (`uint32_t`) | Returns caller's process ID. |
| `SYS_OPEN` | `0x80` | `3` | `ebx` = path, `ecx` = flags, `edx` = mode | fd >= 3 (negative errno on error) | Opens or creates a file on the VFS with specified access flags (`O_CLOEXEC` supported). |
| `SYS_READ` | `0x80` | `4` | `ebx` = fd, `ecx` = buf, `edx` = len | bytes read (negative errno on error) | Reads from fd (file, console stdin, or pipe). |
| `SYS_CLOSE` | `0x80` | `5` | `ebx` = fd | `0` on success, negative errno on error | Closes file descriptor and decrements open_file refcount. |
| `SYS_FORK` | `0x80` | `7` | none | child PID in parent, `0` in child | Clones current process address space, open_files (refcount++), cwd, and CPU registers. |
| `SYS_PIPE` | `0x80` | `8` | `ebx` = int fds[2] | `0` on success, negative errno on error | Creates a 4KB unidirectional IPC pipe and populates read fd (`fds[0]`) and write fd (`fds[1]`). |
| `SYS_EXEC` | `0x80` | `10` | `ebx` = filename, `ecx` = argv | negative errno on error, does not return on success | Replaces caller's address space with target ELF, closes `FD_CLOEXEC` descriptors, and passes `argv`. |
| `SYS_WAITPID` | `0x80` | `11` | `ebx` = pid, `ecx` = status*, `edx` = options | reaped PID / 0 on WNOHANG (negative errno on error) | Waits for child process termination, stores exit code in `*status`, and reclaims zombie resources. |
| `SYS_SBRK` | `0x80` | `12` | `ebx` = increment | previous heap break | Adjusts program break by signed `increment` bytes; pages in memory on demand. |
| `SYS_SYNC` | `0x80` | `13` | none | `0` | Flushes all dirty buffer cache blocks to disk and flushes drive cache. |
| `SYS_LSEEK` | `0x80` | `14` | `ebx` = fd, `ecx` = offset, `edx` = whence | new offset (negative errno on error) | Repositions open file offset (`SEEK_SET`, `SEEK_CUR`, `SEEK_END`). |
| `SYS_STAT` | `0x80` | `15` | `ebx` = path, `ecx` = stat* | `0` on success, negative errno on error | Retrieves file metadata by pathname. |
| `SYS_FSTAT` | `0x80` | `16` | `ebx` = fd, `ecx` = stat* | `0` on success, negative errno on error | Retrieves file metadata by file descriptor. |
| `SYS_GETDENTS` | `0x80` | `17` | `ebx` = fd, `ecx` = dirent*, `edx` = count | bytes read / 0 at EOF (negative errno on error) | Reads directory entries sequentially into dirent structure. |
| `SYS_MKDIR` | `0x80` | `18` | `ebx` = path, `ecx` = mode | `0` on success, negative errno on error | Creates a new directory at specified path. |
| `SYS_RMDIR` | `0x80` | `19` | `ebx` = path | `0` on success, negative errno on error | Removes an empty directory. |
| `SYS_UNLINK` | `0x80` | `20` | `ebx` = path | `0` on success, negative errno on error | Unlinks and deletes a file from directory. |
| `SYS_RENAME` | `0x80` | `21` | `ebx` = oldpath, `ecx` = newpath | `0` on success, negative errno on error | Renames a file or moves it within the filesystem. |
| `SYS_CHDIR` | `0x80` | `22` | `ebx` = path | `0` on success, negative errno on error | Updates caller's current working directory (PCB `cwd`). |
| `SYS_GETCWD` | `0x80` | `23` | `ebx` = buf, `ecx` = size | pointer to buf (negative errno on error) | Retrieves caller's current working directory path string. |
| `SYS_DUP` | `0x80` | `24` | `ebx` = fd | new fd (negative errno on error) | Duplicates fd into lowest available descriptor slot with shared open_file ref. |
| `SYS_DUP2` | `0x80` | `25` | `ebx` = oldfd, `ecx` = newfd | `newfd` (negative errno on error) | Closes `newfd` if open and duplicates `oldfd` onto `newfd`. |
| `SYS_FCNTL` | `0x80` | `26` | `ebx` = fd, `ecx` = cmd, `edx` = arg | cmd result (negative errno on error) | Descriptor control (`F_GETFD`, `F_SETFD`, `F_DUPFD`). |
| `SYS_SLEEP` | `0x80` | `27` | `ebx` = seconds | `0` | Yields CPU until the requested number of seconds has elapsed. |
| `SYS_TICKS` | `0x80` | `28` | none | timer ticks (`uint32_t`) | Returns PIT timer ticks since boot. |
| `SYS_FREE_FRAMES` | `0x80` | `29` | none | free physical frames (`uint32_t`) | Returns count of available physical 4KB memory frames. |

---

## Design Decisions & Known Limitations

### Design Decisions
1. **Pipes as First-Class VFS Open Files**:
   Kernel pipes are represented via `open_file_t` with `OPEN_FILE_PIPE` type. Closing either end unrefs the open_file; when both reader and writer open_files reach zero references, the pipe ring buffer and metadata are freed.
2. **Open File Refcounting & Fork/Dup Sharing**:
   File descriptors point to dynamically allocated `open_file_t` structures. Forking and `dup`/`dup2` increment open_file references, allowing child and parent to share file offsets and pipe ends cleanly.
3. **Wait Queues for Pipe Synchronization**:
   Blocked readers and writers sleep on dedicated wait channels within the pipe struct (`read_wait` and `write_wait`). When buffer space changes or pipe ends close, the opposite wait channel is awakened.
4. **Shell Architecture**:
   The userland shell parses pipelines and redirections, allocating pipes upfront, cloning via `fork()`, applying `dup2()` and closing unused descriptors before `exec()`. Background jobs run asynchronously and zombies are reaped via non-blocking `waitpid()` with `WNOHANG`.
5. **Buffer Cache LRU with Write-Back**:
   A 64-entry x 512-byte buffer cache minimizes disk I/O latency for frequent sector reads/writes. Dirty buffers are flushed to ATA media on `sync()`, reboot, and process exit.
6. **Strict User Pointer Validation**:
   Kernel validates all userspace pointers against mapped pages in the calling process's page directory to prevent kernel memory corruption.

### Known Limitations (at Stage 14 — all resolved in subsequent stages)
1. **Signals (SIGPIPE, SIGCHLD, SIGINT)**:
   *Resolved in Stage 16 & 18B*: Full POSIX signal delivery trampoline, sigaction, sigprocmask, SIGPIPE, and child SIGCHLD reparenting to PID 1.
2. **Job Control**:
   *Resolved in Stage 16*: Terminal process groups (`tcsetpgrp`), sessions (`setsid`), interactive `Ctrl+Z`, `Ctrl+C`, `fg`, `bg`, and `SIGTTIN`/`SIGTTOU`.
3. **LFN (Long File Names)**:
   *Resolved in FAT16 LFN stage*: Full RFC VFAT 255-character Unicode LFN directory parsing, numeric tail generation, and multi-slot creation/deletion.

---

## Stage 15 (done): Memory Management Overhaul

- **Audit & Architectural Design** (`docs/memory.md`): Full architectural specification detailing the higher-half virtual memory transition, direct physical map, VMAs, demand paging, and copy-on-write fork.
- **Physical Memory Manager (PMM) Overhaul** (`src/pmm.c`, `src/pmm.h`):
  - Parses full Multiboot2 memory map, managing all usable RAM across system memory (not restricted to <16MB).
  - Implements per-frame reference counting array (`uint16_t frame_refcount[]`) in early boot memory.
  - APIs: `pmm_alloc_frame()`, `pmm_alloc_contiguous_frames()`, `pmm_ref()`, `pmm_unref()`, `pmm_refcount()`, `pmm_free_count()`, `pmm_total_count()`.
  - Serial logging of total and free RAM at boot.
- **Higher-Half Kernel & Direct Map** (`boot/boot.s`, `boot/linker.ld`, `src/paging.c`, `src/paging.h`, `src/vmm.c`):
  - Memory layout: userspace `0x00000000 - 0xBFFFFFFF` (page 0 unmapped so NULL dereferences fault; user ELFs link at `0x08048000`; stack top at `0xC0000000`).
  - Kernel at `0xC0000000+` (VMA `0xC0100000`, LMA 1MB) with a direct physical map at `0xC0000000 + phys` covering up to 768MB via 4MB PSE pages (`CR4.PSE`).
  - Temporary page directory maps both identity and higher-half at boot, drops identity map in `kernel_main` after paging initialization.
  - Strict user pointer validation range `[0x1000, 0xC0000000)`.
  - Defined `P2V()` / `V2P()` macros and converted all kernel modules, drivers (ATA, bcache, VGA at `0xC00B8000`), and multiboot pointers.
- **Virtual Memory Areas (VMAs)** (`src/vma.c`, `src/vma.h`, `src/process.h`):
  - Per-process sorted list of `vma_t` structs tracking virtual regions (`VMA_ANON`, `VMA_FILE`, `VMA_STACK`, `VMA_HEAP`) with permissions (`PROT_READ`, `PROT_WRITE`, `PROT_EXEC`).
  - Cloned on fork, freed on exec and exit.
  - Page fault handler consults VMAs: invalid accesses or protection violations cleanly terminate the process with exit code 139 and log to serial without panicking the kernel.
  - Safe user-space copy (`copy_to_user` / `copy_from_user` in `boot/uaccess.s`) returns `-EFAULT` on bad pointers instead of kernel panics.
- **Demand Paging & Stack Auto-Growth** (`src/paging.c`, `src/vma.c`, `src/syscall.c`):
  - Lazy allocation on first touch for heap, stack, and ELF BSS / file-backed segments.
  - User stack auto-growth downward up to 8MB (`0xBF800000`) with guard gap.
  - Lazy `sys_sbrk` adjusts heap limit up to 256MB without upfront physical frame allocation; shrinking frees and unrefs pages.
- **Copy-On-Write (COW) Fork** (`src/vmm.c`, `src/paging.c`, `src/process.c`):
  - `fork()` clones user page tables by clearing writable bits and setting software COW bit (bit 9, `PTE_COW`), incrementing frame refcounts.
  - Write fault on COW page allocates a private copy if shared (`refcount > 1`) or restores write permissions if sole owner (`refcount == 1`).
  - Full teardown on `exit()` and `exec()` frees user frames and empty page tables with zero leaks.
- **mmap, munmap & mprotect Syscalls** (`src/syscall.c`, `userland/libc.h`):
  - `SYS_MMAP` (30): Supports `MAP_ANON | MAP_PRIVATE` and file-backed `MAP_PRIVATE`, allocating top-down below stack (`0xB0000000` down).
  - `SYS_MUNMAP` (31): Unmaps pages, unrefs frames, and splits/truncates VMAs.
  - `SYS_MPROTECT` (32): Updates protections across virtual ranges.
  - Userland free-list allocator in `libc.h` on top of `sbrk` with large allocations (>= 64KB) served via `mmap`.
- **Hardened Kernel Heap** (`src/kheap.c`, `src/kheap.h`):
  - Dynamic heap growth from direct physical map (`pmm_alloc_contiguous_frames`).
  - Bidirectional block coalescing.
  - Per-block magic headers (`0xDEADBEEF`) and tail canaries (`0xCAFEBABE`) detecting buffer overflows and double-frees immediately.
  - Boot-time self-test (`kheap_selftest()`) verifying stress allocations, coalescing, and dynamic growth.
- **Comprehensive Memory Test Suite** (`userland/memtest.c`, `tools/test_boot.sh`):
  - Validates COW correctness, COW efficiency, demand paging, stack growth, mmap/munmap, protection violations (NULL, code write, non-exec, kernel address), and fork storm (200 sequential + 20 concurrent children with zero leaks).
  - Validated across QEMU `-m 256` and `-m 64` configurations.

---

## Stage 16 (done): TTY, Signals, Job Control, Time & Timers, ProcFS, and Scheduler

- **Device Nodes & In-Memory DevFS** (`src/devfs.c`, `src/devfs.h`):
  - In-memory `/dev` filesystem supporting `/dev/console`, `/dev/tty`, `/dev/null`, `/dev/zero`, `/dev/serial0`.
  - Stdin/stdout/stderr for PID 1 (and inherited children) open `/dev/console` through the VFS abstraction.
- **TTY Subsystem & Line Discipline** (`src/tty.c`, `src/tty.h`):
  - Struct `tty_t` maintaining input ring buffer, line discipline state, foreground process group ID (`pgrp`), window size (80x25), and POSIX termios flags (`ICANON`, `ECHO`, `ISIG`, etc.).
  - Input multiplexing: Keyboard IRQ1 and COM1 serial IRQ feed the active TTY line discipline.
  - Canonical line editing: backspace, Ctrl+U (kill line), Ctrl+W (kill word), line buffered until newline, Ctrl+D (EOF on empty buffer).
  - Raw / non-canonical mode via `SYS_IOCTL` (`TCGETS`, `TCSETS`, `TIOCGWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`).
- **POSIX Signal Infrastructure** (`src/signal.c`, `src/signal.h`, `boot/isr.s`):
  - 32 signal types with standard default actions (Terminate, Ignore, Stop, Continue).
  - Syscalls: `SYS_SIGACTION` (38), `SYS_SIGPROCMASK` (39), `SYS_SIGRETURN` (40), `SYS_KILL` (41), `SYS_ALARM` (42), `SYS_PAUSE` (43).
  - Signal delivery trampoline: Kernel constructs userspace signal frame on user stack, saves interrupted registers and blocked mask, invokes user handler, and returns through `sigreturn`.
  - `SIGALRM` driven by PIT timer callbacks waking `pause()`.
  - Page faults outside VMAs deliver `SIGSEGV` instead of halting the system.
  - Broken pipe writes deliver `SIGPIPE`.
- **Job Control & Terminal Process Groups** (`src/scheduler.c`, `userland/sh.c`):
  - Process group management: `SYS_SETPGID` (44), `SYS_GETPGRP` (45), `SYS_SETSID` (46).
  - Background process protection: background jobs reading from foreground TTY are stopped with `SIGTTIN`; background writes with `TOSTOP` are stopped with `SIGTTOU`.
  - Shell interactive job control: `jobs`, `fg`, `bg`, `kill %[n]`, parsing `&`, tracking Running and Stopped jobs, with non-blocking child notification via `SIGCHLD`.
  - Signal keys: Ctrl+C sends `SIGINT` to foreground group; Ctrl+Z sends `SIGTSTP`; Ctrl+\ sends `SIGQUIT`.
- **CMOS RTC & Wall Time** (`src/rtc.c`, `src/rtc.h`):
  - CMOS Status Register A UIP polling, consecutive match reading, Status B BCD / 12h-24h decoding, century register 0x32 with fallback to 20xx.
  - Wall time tracked in UTC epoch seconds internally; updated via PIT ticks.
  - `/etc/timezone` configured with minute offset (e.g. 330 for IST UTC+5:30); default 0.
  - Userland `date` prints local time formatted with timezone; `date -u` prints UTC time.
  - FAT16 directory operations (`create`, `mkdir`, `write`, `rename`) stamp UTC timestamps; `stat` populates `st_mtime`; `ls -l` formats timestamps in local time.
- **Non-Busy Sleeping Wait-Queue & CPU Accounting** (`src/timer.c`, `src/timer.h`):
  - Sorted timer wait queue processed on PIT IRQ (deadline-based wakeups; O(1) tick check).
  - Sleeping processes reside in `PROCESS_WAITING` consuming 0% CPU; idle task executes `hlt`.
  - `nanosleep` and `sleep` are signal-interruptible, immediately dequeued from wait list and returning `-EINTR` with remaining time struct.
  - Accurate per-process tick accounting (`cpu_ticks` only incremented when process actually executes).
- **ProcFS-Lite** (`src/procfs.c`, `src/procfs.h`):
  - Dynamic in-memory VFS mounted at `/proc`.
  - `/proc/meminfo`: system memory and frame allocation statistics.
  - `/proc/uptime`: system uptime and idle time in seconds.
  - `/proc/<pid>/status`: process name, PID, PPID, PGID, SID, state, cpu ticks, VM pages, open FD count.
  - `/proc/<pid>/cmdline` and `/proc/self` symlink node.
  - Directory listing of `/proc` dynamically enumerates active PIDs.
  - User utilities: `ps`, `uptime`, `free`, and `top`-lite built as standard procfs consumers.
- **Scheduler Nice Values & Responsiveness** (`src/scheduler.c`, `userland/sh.c`):
  - Per-process nice value (-20 to 19) mapping to dynamic time slice ticks (1 to 20 ticks).
  - Shell and interactive processes remain responsive (<1s prompt response) even under multiple CPU-bound compute loops (`spin.elf`).
- **Comprehensive Validation Test Suites**:
  - `userland/sigtest.c`: Tests signal delivery, sigprocmask blocking, sigaction handlers, SIGALRM/pause, SIGPIPE, SIGSEGV on faults, SIGCONT/SIGSTOP, and signal interruption of `nanosleep`.
  - `userland/timetest.c`: Tests non-busy sleep tick duration (190-230 ticks for 2s), sleeper CPU ticks (< 5), and procfs memory statistics match.
  - `tools/test_boot.sh` (Sessions 1-7): End-to-end regression testing covering VFS persistence, IPC/pipes, memory management, TTY raw/canonical modes, live job control (`spin.elf`, `fg`, `bg`, pipelines, Ctrl+C/Z), RTC time, and ProcFS with zero frame leaks.

---

## Stage 17 (done): Cross Toolchain, Newlib Libc, FPU/SSE, and Ported Software (Lua 5.4, Kilo)

- **Kernel FPU / SSE Architecture** (`src/fpu.c`, `src/fpu.h`, `src/process.c`, `src/process.h`):
  - Hardware CPUID detection for FPU, FXSR, SSE, SSE2, and AVX capabilities.
  - Boot initialization: executes `fninit`, enables `CR4.OSFXSR` and `CR4.OSXMMEXCPT`, clears `CR0.EM`, sets `CR0.MP`.
  - Per-process 512-byte 16-byte aligned FXSAVE area embedded within `process_t`.
  - Context switching: saves and restores userland FPU/SSE state using `fxsave` and `fxrstor`.
  - `fork()` clones active FPU state; `exec()` resets FPU state to clean defaults; signal trampoline preserves FPU state in user signal frames.
  - Exception routing: #MF (vector 16) and #XM (vector 19) map to `SIGFPE`.
  - Verified through `userland/fputest.c`: 3 concurrent processes and forked children performing high-iteration floating-point and SSE math across context switches without state corruption.

- **Cross Toolchain Build System** (`tools/toolchain/build.sh`, `tools/myos-gcc`):
  - Builds pinned GNU Binutils 2.42, GCC 13.2.0 (C only), and Newlib 4.4.0 targeting `i686-elf` to `$HOME/opt/myos-cross`.
  - Non-interactive, idempotent with post-install verification markers (`$PREFIX/.built-<pkg>`) and automatic source/build cleanup.
  - Smoke tests in `tools/toolchain/smoke.sh` verifying pinned versions, elf32-i386 output, and math/malloc/setjmp linking.
  - Compiler wrapper `tools/myos-gcc`: passes default MyOS CFLAGS (`-m32 -march=i686 -fno-pie -fno-stack-protector`), includes Newlib C headers and `libmyos` headers, links `crt0.o`, `libmyos.a`, `libc.a`, `libm.a`, and `libgcc.a`.

- **POSIX Emulation & Syscall Glue: `libmyos`** (`libmyos/`):
  - Implements Newlib system call stubs in `libmyos/stubs.c` (`_open`, `_close`, `_read`, `_write`, `_lseek`, `_fstat`, `_stat`, `_sbrk`, `_getpid`, `_kill`, `_fork`, `_execve`, `_wait`, `_times`, `_gettimeofday`, `_unlink`, `_rename`).
  - Implements POSIX API layer in `libmyos/posix.c`: `nanosleep`, `clock_gettime`, `tcgetattr`, `tcsetattr`, `ioctl`, `sigaction`, `sigprocmask`, `sigreturn`, `opendir`, `readdir`, `closedir`, `getcwd`, `chdir`, `dup`, `dup2`, `fcntl`, `pipe`, `waitpid`, `setenv`, `unsetenv`, `getenv`, `mmap`, `munmap`, `flockfile`, `popen`, `pclose`, `getline`, `getdelim`.
  - Translates kernel 32-bit types (e.g. `struct timespec`, `struct timeval`, `struct dirent`) to Newlib's 64-bit `time_t` layout and standard POSIX structs.
  - Setjmp forwarders in `libmyos/setjmp_alias.S` bridging `_setjmp`/`_longjmp` to Newlib's assembly `setjmp`/`longjmp`.

- **Environment Variables End-to-End**:
  - `execve` serializes `char *const envp[]` into user space below `argv`.
  - `crt0.S` extracts `environ` pointer and initializes Newlib's global `environ`.
  - `getenv`, `setenv`, and `unsetenv` supported end-to-end with inheritance across `fork` and `exec`.

- **Real Software Ported**:
  - **Lua 5.4.6** (`userland/lua.elf`, `userland/luac.elf`): Full ANSI Lua 5.4 bytecode compiler and interpreter with standard libraries (`math`, `string`, `table`, `io`, `os`), REPL on TTY, and execution of scripts like `/home/demo.lua`.
  - **Kilo Editor** (`userland/edit.elf`): Text editor with syntax highlighting, ANSI escape codes, terminal raw mode, window size detection (`TIOCGWINSZ`), and disk file saving.
  - **Demo Script** (`/home/demo.lua`): Demonstrates Fibonacci recursion, libm / FPU math (`sin`, `sqrt`, `exp`, `log`), string manipulation, table sort benchmark, disk file I/O, and real-time clock dates.

- **Comprehensive Test Suite & Session 8 Validation**:
  - `userland/libctest.c`: Exhaustive test suite verifying printf/sprintf/snprintf formats, sscanf, stdio file I/O, malloc/calloc/realloc/free pattern stress, qsort/bsearch, strtol/strtoul/strtod/atof, string/ctype functions, setjmp/longjmp, getenv/setenv, time/strftime/localtime, libm math functions (sin, cos, tan, sqrt, exp, log, pow, floor, ceil, fmod), errno mapping, opendir/readdir, fork/pipe/waitpid, sigaction, and anonymous mmap.
  - `tools/test_boot.sh` Session 8: Validates `libctest`, Lua one-liner expressions, `/home/demo.lua`, piped Lua streaming (`ls /bin | lua ... | head`), Lua syntax error handling (exit 1), Lua stack overflow detection on deep recursion without kernel panic, 20MB table allocation/collection, and zero physical frame leaks.

---

## Stage 18A (done): Graphics Foundation

- **Multiboot2 Framebuffer & PAT Write-Combining**:
  - 1024x768 32bpp linear framebuffer boot negotiated via Multiboot2 tags 5 and 8.
  - Page Attribute Table (PAT) write-combining MSR configuration and `ioremap` at virtual range `0xF0000000-0xFFBFFFFF`.
  - Automatic fallback to standard 80x25 VGA text console when framebuffer is unavailable (`-vga none`).

- **Graphical Text Console & PSF2 Font**:
  - 128x48 character grid at 1024x768 with embedded Terminus 8x16 PSF2 bitmap font.
  - Dirty-row RAM shadow buffer preventing slow reads from video memory during scrolling and rendering.
  - Full ANSI escape sequence support (colors, bold, cursor movement, erase commands).
  - Virtual terminal control ioctls (`KDSETMODE` for `KD_TEXT` vs `KD_GRAPHICS`, `KDSKBMODE` for `K_XLATE` vs `K_OFF`).

- **Input Subsystem & Raw Event Ring Buffer**:
  - PS/2 keyboard and IntelliMouse driver with 4-byte wheel stream and absolute mouse coordinate clamping.
  - Linux-compatible device nodes: `/dev/input/mice`, `/dev/input/mouse0`, `/dev/input/kbd0` delivering `struct input_event` structures.
  - Kernel-side emergency restore: `Ctrl + Alt + F1` resets keyboard translation to `K_XLATE`.

- **Framebuffer Device `/dev/fb0`**:
  - Screen geometry query ioctls: `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`.
  - Page-granular device `mmap()` with write-combining attributes allowing direct zero-copy userland rendering.

- **Non-blocking I/O & Multiplexing**:
  - `poll()` system call and `select()` wrapper in `libmyos` supporting pipes, TTY, and input devices.
  - Non-blocking I/O flag support (`O_NONBLOCK`).

- **2D Graphics Library (`libgfx`)**:
  - Fast Bresenham lines, rectangles, circles, rounded rectangles, gradients, and alpha blending (`ARGB` source-over compositing).
  - Embedded PSF2 console bitmap font and scalable TrueType font rendering via vendored `stb_truetype.h` (`DejaVuSans.ttf`).
  - Multi-format image loading (BMP, PNG, JPEG) via `stb_image.h` and optimized single-buffer BMP exporter (`gfx_save_bmp`).
  - Unified input event pump (`gfx_poll_event`, `gfx_wait_event`) with automatic software cursor overlay.

- **Demo Applications**:
  - `gfxdemo`: Animated gradient and bouncing physics balls, plus deterministic `--still` mode with exact reference color patches for automated pixel testing.
  - `paint`: Multi-tool drawing program (pencil, line, rect, fill_rect, circle, fill_circle) with 14-color palette, canvas clearing, and BMP saving (`s` key).
  - `view`: Multi-format image viewer supporting BMP, PNG, and JPEG.
  - `screenshot`: Direct framebuffer capture utility exporting 24-bit BMP to disk.
  - `crash_safety_test`: Switches to `KD_GRAPHICS` and `K_OFF` and triggers page fault to prove kernel cleanup and console recovery.

- **Automated Verification & QEMU Monitor (`tools/qmon.py`, `tools/test_step8.py`)**:
  - Session 12 automated test suite verifying screendump framebuffers, RGB byte order, ESC console restoration, raw mouse and keyboard event injection, and `-vga none` text fallback boot.

---

## FAT16 Long Filenames (LFN) & Concurrency Architecture (done)

- **Sleeping Mutex Subsystem (`src/kmutex.c`, `src/kmutex.h`, `src/scheduler.c`)**:
  - Implements non-spinning FIFO sleeping mutexes (`kmutex_t`).
  - Processes contending for an acquired mutex block cleanly on a wait queue via `scheduler_wait_or_idle()`, consuming 0% CPU without burning cycles in spinloops.
  - Dynamically asserts lock ownership (`owner == scheduler_current()`) and validates that no sleeping mutexes are held when a process exits (`process_exit`).
  - Added lock leak checks and interrupt flag assertions (`assert_interrupts_disabled()`) on all syscall boundaries.

- **Elimination of Interrupt Latency During Disk I/O (`src/bcache.c`, `src/fat16.c`)**:
  - Migrated buffer cache (`bcache`) and FAT16 filesystem operations from disabling spinlocks to sleeping mutexes (`bcache_mutex`, `fat16_mutex`).
  - Enabled CPU interrupts (`sti`) during multi-millisecond ATA PIO sector transfer loops (`inw`/`outb`).
  - Completely eliminated timer tick starvation and keyboard/mouse dropped packets during heavy disk read/write bursts.
  - Converted static `fat_node_t` pool to dynamic per-node heap allocation (`kmalloc`/`kfree`) with VFS cleanup callbacks, eliminating fixed pool exhaustion.

- **VFAT Long Filenames (LFN) Subsystem (`src/fat16.c`, `src/fat16.h`)**:
  - Full RFC-compliant VFAT Long Filename implementation with support for filenames up to 255 characters and mixed case.
  - Multi-slot LFN directory entry parsing and contiguous slot allocation.
  - 8.3 numeric tail alias generation (`NAME~1.EXT` through `NAME~999999.EXT`) with collision detection.
  - UTF-16LE to ASCII/UTF-8 character conversion with checksum verification across all directory slots.
  - Multi-slot atomic deletion and renaming in `unlink()`, `rmdir()`, and `rename()`.
  - Case-insensitive directory lookup matching both LFN and short 8.3 aliases.

- **ProcFS & Kernel Telemetry Additions (`src/procfs.c`, `src/procfs.h`)**:
  - `/proc/cpuinfo`: Reports CPU vendor, family, model, stepping, CPUID feature flags (FPU, PSE, MSR, CX8, APIC, SEP, MTRR, PGE, MCA, CMOV, PAT, CLFSH, DS, ACPI, MMX, FXSR, SSE, SSE2, SS, HTT, TM, PBE), and clock frequency estimate.
  - `/proc/version`: Exposes kernel version string, compiler build info, and target architecture.
  - `/proc/cmdline`: Exposes Multiboot2 boot command-line arguments.

- **Core POSIX Utilities & Shell Builtins (`userland/`, `userland/sh.c`)**:
  - `uname`: Implements standard system information tool supporting `-a`, `-s`, `-r`, `-m`, `-v`, `-n`.
  - `which`: Command resolution searching directories in `PATH`.
  - `clear`: ANSI terminal screen clearing.
  - `basename` / `dirname`: Path component extraction and suffix stripping.
  - `seq`: Sequence generator with start, step, and end parameters.
  - `cal`: Gregorian calendar generator using CMOS RTC and libc time.
  - `env`: Environment variable viewer and runner.
  - `nl`: Filter adding line numbers to input streams.
  - Shell builtins: Added `which`, `clear`, and `version` directly into `sh`.

- **Automated Concurrency & LFN Validation Suite**:
  - `userland/lfntest.c`: 35-second test suite validating LFN creation, multi-slot directory extension, numeric tail alias generation, case insensitivity, deep directory recursion, multi-slot unlinking, and renaming.
  - `userland/lfnstress.c`: Model-based fuzzing and stress testing executing 500 pseudo-random filesystem operations (create, write, read, rename, delete) followed by host-side `fsck.fat -v -a` and `mdir` verification.
  - `userland/ticktest.c`: 20-second timer tick benchmark running under heavy concurrent ATA disk I/O, proving zero timer tick loss.
  - `userland/concur_test.c`: 4 concurrent child processes executing simultaneous disk operations with preemption spinners without data corruption or kernel panics.

---

## Stage 18B (done): Kernel IPC, Session Infrastructure & Process Tree

- **`tmpfs` In-Memory Filesystem & Flexible VFS Mounts (`src/tmpfs.c`, `src/tmpfs.h`, `src/mount.c`)**:
  - Dynamic page-backed in-memory filesystem mounted at `/dev/shm` and `/run` with 63MB allocation caps.
  - Supports sparse page arrays, directory trees, `S_IFSOCK` socket nodes, `S_IFCHR` device nodes, and file truncation (`truncate`, `ftruncate`).
  - Dynamic VFS mount table exposed via `/proc/mounts` in standard `/etc/mtab` format (`fat16`, `devfs`, `procfs`, `tmpfs`).

- **Shared Memory (`MAP_SHARED`) & POSIX SHM (`src/paging.c`, `src/vmm.c`, `src/vma.c`, `src/syscall.c`, `libmyos/`)**:
  - Unified page-table-level shared memory supporting anonymous mappings (`MAP_SHARED | MAP_ANON`) and tmpfs file-backed mappings.
  - Shared page tables across `fork()`: parent and child map identical physical frames with `PAGE_WRITABLE` (no Copy-On-Write duplication), enabling atomic synchronization.
  - Beyond-EOF access protection: attempting to touch mapped addresses past `st_size` delivers `SIGBUS` (signal 7) to the faulting process.
  - POSIX shared memory APIs in `libmyos`: `shm_open`, `shm_unlink`, and `msync`.

- **UNIX Domain Sockets (`AF_UNIX`) (`src/socket.c`, `src/socket.h`, `src/syscall.c`)**:
  - Bidirectional byte-stream IPC (`SOCK_STREAM`) with 64KB kernel ring buffers (`UNIX_SOCKET_BUF_SIZE = 65536`).
  - Filesystem rendezvous nodes (`S_IFSOCK`) created via `bind()` on `tmpfs` mounts (e.g. `/run/display.sock`).
  - Syscalls: `SYS_SOCKET` (47), `SYS_BIND` (48), `SYS_LISTEN` (49), `SYS_ACCEPT` (50), `SYS_CONNECT` (51), `SYS_SHUTDOWN` (52).
  - Telemetry: Dynamic socket table exposed via `/proc/net/unix` and open file descriptors listed under `/proc/<pid>/fd`.

- **UNIX98 Pseudoterminals (PTY) & Multiple TTYs (`src/pty.c`, `src/pty.h`, `src/devfs.c`, `libmyos/`)**:
  - Master multiplexer device node `/dev/ptmx` and dynamic slave nodes `/dev/pts/<N>`.
  - Full bidirectional line discipline integration on slave PTYs (canonical input editing, `ICRNL`, `ECHO`, `ONLCR`).
  - Window resizing ioctls (`TIOCSWINSZ` / `TIOCGWINSZ`) delivering `SIGWINCH` signals to the foreground process group.
  - Libc APIs in `libmyos`: `posix_openpt()`, `grantpt()`, `unlockpt()`, `ptsname_r()`, `openpty()`, and `forkpty()`.
  - Kernel command-line `serial_shell=1` argument in `/proc/cmdline` spawning an independent shell on `/dev/serial0`.

- **Process Hierarchy & Init Supervisor (`src/process.c`, `userland/init.c`)**:
  - Process orphan reparenting: when any process terminates, its surviving child processes are automatically reparented to PID 1 (`/sbin/init`), with `SIGCHLD` signals delivered to the new parent.
  - Init supervisor (`/sbin/init`) parsing `/etc/init.conf` with `once`, `wait`, and `respawn` directives.
  - Rate-limiting protection: tracks service spawn timestamps and caps restarts to 5 within 10 seconds, applying a 5-second backoff to prevent fork storms.
  - Non-blocking zombie reaping via `waitpid(-1, &status, WNOHANG)`.

- **Kernel Power Subsystem & Clean Shutdown (`src/kernel.c`, `src/syscall.c`, `userland/power.c`)**:
  - ACPI shutdown primitive via I/O ports (`0x604`, `0xB004`, `0x4004`, `0x600`).
  - 8042 keyboard controller reset primitive (`outb(0x64, 0xFE)`) and IDT null triple-fault fallback for reboot.
  - Syscall: `SYS_REBOOT` (53) supporting `LINUX_REBOOT_CMD_POWER_OFF`, `LINUX_REBOOT_CMD_RESTART`, and `LINUX_REBOOT_CMD_HALT`.
  - Graceful shutdown sequence: broadcasts `SIGTERM` to all user processes, waits up to 2 seconds, sends `SIGKILL` to stragglers, synchronizes dirty buffer cache sectors to disk (`bcache_sync`), and executes power action.
  - Userland utilities: `poweroff`, `reboot`, `halt`, and `shutdown`.
  - GRUB dual boot configurations in `boot/grub.cfg` for text mode vs. graphical desktop sessions.

- **Comprehensive Stage 18B Verification**:
  - `userland/shmtest.c`: Validates shared memory across `fork()`, cross-process atomics, tmpfs file-backed mappings, and beyond-EOF `SIGBUS` delivery.
  - `userland/sockettest.c`: Validates `socketpair`, client-server stream connection, 64KB ring buffer throughput, `select()` polling, non-blocking modes, and `/proc/net/unix` inspection.
  - `userland/ptytest.c`: Validates master/slave PTY pair allocation, bidirectional transmission, canonical line buffering, `TIOCSWINSZ` window size delivery, and `SIGWINCH` signal receipt.
  - `userland/ipctest.c`: Comprehensive test suite integrating tmpfs mounts, truncate, shared memory, domain sockets, PTYs, and power management.
  - `tools/test_boot.sh` (Sessions 11 & 13): Automated regression tests asserting clean poweroff and reboot with dirty filesystem buffer flush.

---

## Stage 18B-2 (done): Window System & Desktop Environment

- **Architecture & IPC Protocol (`docs/window_system.md`, `userland/libwin/win_proto.h`)**:
  - Client-server architecture communicating over AF_UNIX stream socket `/run/display.sock`.
  - Fixed 12-byte binary framing header: `{uint32_t len, uint16_t type, uint16_t flags, uint32_t window_id}` (little-endian, max 4096 bytes).
  - Out-of-band shared memory surface buffers: clients allocate double-buffered framebuffers under `/dev/shm/win-<pid>-<win_id>-<buf_idx>` via `shm_open()` and attach them to the window manager via `WIN_MSG_ATTACH_BUFFER`.
  - Asynchronous event flow: clients send damage rectangles (`WIN_MSG_DAMAGE`) and commit frames (`WIN_MSG_COMMIT`); compositor delivers keyboard (`WIN_MSG_KEY`), mouse move/button/wheel (`WIN_MSG_POINTER_*`), focus changes (`WIN_MSG_FOCUS`), and configure events (`WIN_MSG_CONFIGURE`).

- **Display Server & Window Manager (`wm`) (`userland/wm.c`)**:
  - Takes exclusive display ownership via `KD_GRAPHICS` and disables console translation via `K_OFF`.
  - Composes client windows into a 32bpp RAM back buffer with dirty rectangle tracking, flushing only damaged sub-regions to `/dev/fb0`.
  - Renders server-side window decorations: titlebar, window title text, close button, minimize button, maximize button, and drop shadows.
  - Translates raw scancodes into Unicode characters using `/usr/share/keymaps/us.map`.
  - Software mouse cursor overlay with automatic background restoration.
  - Interactive window management: titlebar mouse drag-to-move, border resizing, and z-order restacking.
  - Keyboard window switcher: `Alt + Tab` overlay displaying active window thumbnails/titles.
  - Fault isolation: automatically cleans up destroyed or disconnected clients; protects compositor against truncated shared memory surfaces via `SIGBUS` handlers.

- **Window Client Library (`libwin`) (`userland/libwin/`)**:
  - High-performance client library providing window creation, surface allocation (`gfx_surface_t`), damage tracking, atomic frame commits, and event dispatch loops (`win_dispatch_event`, `win_poll_event`, `win_wait_event`).
  - Integrated clipboard management (`WIN_MSG_CLIPBOARD_SET`, `WIN_MSG_CLIPBOARD_GET`).

- **Window Management Control CLI (`wmctl`) (`userland/wmctl.c`)**:
  - Command-line tool communicating with `wm` over `/run/display.sock`.
  - Operations: list active windows (`wmctl -l`), switch focus (`wmctl -f <id>`), close window (`wmctl -c <id>`), move window (`wmctl -m <id> <x> <y>`), minimize window (`wmctl -i <id>`), query compositor stats (`wmctl -s`), and capture full-screen screendumps to BMP (`wmctl -d <path>`).

- **GUI Widget Toolkit (`libui`) (`userland/libui/`)**:
  - Retained-mode hierarchical widget toolkit with parent-child layout management, dirty rectangle invalidation, and event bubbling.
  - Keyboard focus traversal via `Tab` and `Shift + Tab`.
  - **Core Controls**:
    - `ui_label`: Static text labels with alignment and custom colors.
    - `ui_button`: Push buttons with hover, active, and focus states and click callbacks.
    - `ui_checkbox`: Checkbox controls with toggled states and label captions.
    - `ui_radio_group`: Mutually exclusive radio option groups.
    - `ui_progress_bar`: Progress indicators supporting determinate (0-100%) and indeterminate modes.
  - **Text Controls**:
    - `ui_textbox`: Single-line text input with cursor blinking, text selection, clipboard copy/cut/paste, backspace/delete, and enter callbacks.
    - `ui_textarea`: Multiline text editor with word wrap, vertical scrolling, cursor positioning, and selection.
  - **Advanced Controls**:
    - `ui_listbox`: Scrollable item lists with item selection callbacks.
    - `ui_scrollbar`: Standalone vertical and horizontal scrollbars.
    - `ui_tab_control`: Multi-page notebook tab container.
    - `ui_menubar`: Application menubars with dropdown menus and keyboard accelerators.
    - `ui_dialog`: Modal message boxes and confirmation dialogs.

- **Desktop Applications & Showcases**:
  - **`uidemo` (`userland/uidemo.c`)**: Comprehensive demonstration application showcasing all `libui` widgets across multiple tabbed views (buttons, textboxes, textareas, checkboxes, radio groups, listboxes, progress bars, and dialogs).
  - **`winhello` (`userland/winhello.c`)**: Basic window system client demonstrating protocol handshakes, buffer attachment, damage reporting, and event handling.
  - **`term` (`userland/term.c`)**: Graphical terminal emulator running atop `libwin` and PTYs (`forkpty`), featuring VT100 / ANSI escape sequence parsing, xterm-256color support, smooth scrolling, and shell session integration (`/bin/sh`).
  - **`files` (`userland/files.c`)**: Graphical file manager featuring directory navigation, path breadcrumbs, VFAT LFN directory listings, item counters, status bar, and file deletion.

- **Automated Verification Suites (`tools/test_step*.py`)**:
  - `tools/test_step1.py`: Protocol handshake, buffer attachments, damage reporting, and `winhello` rendering verification.
  - `tools/test_step2.py`: Server-side decorations, mouse dragging, resizing, focus restacking, and `wmctl` actions.
  - `tools/test_step3.py`: Complete `libui` widget suite validation and `uidemo` interactions.
  - `tools/test_step4.py`: Graphical terminal emulator (`term`) VT100 escape code rendering, PTY input echo, and 256-color palette verification.
  - `tools/test_step5a.py`: Graphical file manager (`files`) directory navigation, LFN rendering, and file operations.

---

## Stage 19 (done): AI-Native OS Core — First-Class Kernel Agents

Stage 19 introduces Phase 1 of an AI-Native Operating System: transforming AI and autonomous tasks into first-class kernel objects with hardware-enforced boundaries, default-deny capability allowlists, resource budgets, and cryptographic audit trails.

- **Kernel Agent Subsystem (`src/agent.h`, `src/agent.c`)**:
  - `agent_t` structure tracking `id`, `name`, `owner_pid`, `parent_agent_id`, `state` (`RUNNING`, `SUSPENDED`, `KILLED`), capability set, resource budgets, and consumption counters.
  - Global agent table of up to 32 concurrent agent contexts synchronized via non-spinning sleeping mutex (`kmutex_t`).
  - Process binding: `process_t.agent_id` links processes to agents. Children inherit agent bindings on `fork()`; bindings are strictly preserved across `execve()`. Non-agent processes (`agent_id = 0`) execute unconfined with zero overhead.
- **Default-Deny Capability Model**:
  - Bitmask permissions (`CAP_FS_READ`, `CAP_FS_WRITE`, `CAP_EXEC`, `CAP_SOCKET_CONNECT`, `CAP_WINDOW`, `CAP_SPAWN`, `CAP_NET_PROXY`).
  - Canonical path-prefix allowlists (`fs_read`, `fs_write`, `exec`, `connect`) evaluated against process working directories via `vfs_path_canonical()`.
  - Enforced directly within kernel system calls: `open`, `unlink`, `rename`, `mkdir`, `rmdir`, `execve`, `connect`, `fork`. Every violation returns `-EACCES`, triggers an audit log event, and is logged to serial (`kprintf`).
  - Strict delegation narrowing: sub-agents can only be delegated a subset of the parent agent's capability bitmask and path prefixes; privilege widening is mathematically blocked and denied.
- **Resource Budgets & Automatic Suspension**:
  - Per-agent budgets: `max_cpu_ticks`, `max_mem_pages`, `max_procs`, `max_tokens`.
  - Dynamic charging: PIT timer interrupt charges CPU ticks. Upon budget exhaustion, the agent transitions to `AGENT_STATE_SUSPENDED`, descheduling its processes while leaving the shell and desktop interactive.
  - Fork storm prevention: `fork()` checks `max_procs` and fails with `-EAGAIN` once the limit is reached.
- **Cryptographic Tamper-Evident Audit Trail**:
  - 64KB kernel ring buffer holding up to 256 structured audit entries (`seq`, `agent_id`, `tick`, `wall_time`, `event_type`, `detail`).
  - Cryptographic hash chaining: in-kernel FIPS 180-2 SHA-256 implementation (`src/sha256.c`, `src/sha256.h`) computes $\text{hash}_n = \text{SHA-256}(\text{hash}_{n-1} \parallel \text{payload}_n)$.
  - Exposed via virtual filesystem at `/proc/agents/log`, `/proc/agents/<id>/{status,caps,budget}`, and `/proc/agents`.
- **System Calls & Userland Tooling**:
  - New system calls: `SYS_AGENT_CREATE` (170), `SYS_AGENT_ATTACH` (171), `SYS_AGENT_KILL` (172), `SYS_AGENT_SUSPEND` (173), `SYS_AGENT_RESUME` (174), `SYS_AGENT_INFO` (175). Wrappers in `libmyos.a` (`libmyos/posix.c`, `libmyos/include/agent.h`).
  - Management CLI `agentctl` (`userland/agentctl.c`): `create`, `list`, `log [-f]`, `kill`, `suspend`, `resume`, `run`.
  - Comprehensive automated test suite `agenttest` (`userland/agenttest.c`): validates capability allowlists, delegation narrowing, CPU limits, fork storm limits, SHA-256 chain verification, and zero physical frame leaks.

---

## Stage 20 (done): AI-Native OS Phase 2 — The Intent Bus

Stage 20 introduces Phase 2 of an AI-Native Operating System: the **Intent Bus**, providing secure, authenticated inter-agent skill registration and invocation over Unix domain sockets (`/run/intent.sock`) with kernel peer authentication, fine-grained capability checks, schema validation, sensitive skill approvals, and privacy-preserving audit logging.

- **Bus Daemon (`intentd`, `userland/intentd.c`)**:
  - Event-driven, single-threaded `poll()` daemon listening on `/run/intent.sock`, launched as `respawn` in `/etc/init.conf`.
  - Privileged registration via `SYS_BUS_DAEMON_REGISTER` (177) restricted to the init process tree.
  - In-kernel audit logging via `SYS_AUDIT_APPEND` (178) appending `BUS_REGISTER`, `BUS_CALL`, `BUS_DENIED`, and `BUS_APPROVAL` directly into the SHA-256 tamper-evident hash chain.
  - Per-caller rate limiting (50 calls/sec) and in-flight concurrency caps (max 8) preventing bus exhaustion or starvation.
  - Bounded 5s provider timeouts and instant mid-call cleanup on provider termination without hung callers.
- **Kernel Peer Authentication (`SYS_SOCK_PEERINFO`)**:
  - `SYS_SOCK_PEERINFO` (176) captures immutable `{pid, agent_id}` at `connect()`/`accept()` time, eliminating caller spoofing or body impersonation attacks.
- **Bus Capabilities (`CAP_BUS_CALL`, `CAP_BUS_PROVIDE`)**:
  - Fine-grained prefix allowlists (`fs.`, `sys.`) stored in `agent_caps_t` and exposed in `/proc/agents/<id>/caps`.
  - Narrowing rules enforced on agent delegation; managed via `agentctl create --bus-call <pfx> --bus-provide <pfx>`.
- **Wire Protocol & Lightweight JSON Engine (`libjson`, `userland/libjson/`)**:
  - 12-byte binary framing header `{len, type, flags, call_id}` with strict UTF-8 JSON bodies.
  - Small, zero-malloc-in-hot-path JSON parser and serializer with bounded recursion depth (<= 16) and JSON Schema type validation.
- **Client Libraries & Language Bindings (`libintent`, `userland/libintent/`)**:
  - C client library: `intent_connect`, `intent_register_skill`, `intent_serve`, `intent_call`, `intent_list_skills`, `intent_list_approvals`, `intent_resolve_approval`.
  - Lua 5.4 module binding (`lua_intent.c`): `intent.register()`, `intent.call()`, `intent.serve()`, `intent.list()`.
- **Management CLI & Demo Skills (`intentctl`, `demoskills`)**:
  - `intentctl`: `list`, `call <skill> '<args>'`, `approvals`, `approve <id>`, `deny <id>`, `stats`.
  - Demo skills: `fs.list`, `fs.read` (confined to `/home`), `sys.meminfo`, and sensitive `fs.delete` (confined to `/home/out`).
- **Comprehensive Verification Suite (`intenttest`)**:
  - Automated test suite validating round-trip invocation, schema rejection, malformed fuzzing resilience, capability denial, confused deputy defense against kernel sandbox, approval workflow, crash handling, rate limits, audit hash chain verification, and zero frame/fd leaks over 200 cycles.

---

## Stage 21 (done): AI-Native OS Phase 3 — Model Daemon, Host Bridge, Token Budgets & Autonomous Agent Loop

Stage 21 introduces Phase 3 of the AI-Native Operating System: turning LLM inference into a governed, budgeted OS service. Agents invoke model completion, embedding, and status skills via the Intent Bus, while the kernel and daemons guarantee that host API credentials never cross the VM boundary, token budgets are strictly enforced, and prompt injections are quarantined.

- **Raw Second Serial Device (`/dev/serial1`)**:
  - COM2 raw, non-TTY byte device (`0x2F8`, IRQ3) with an 8KB circular RX buffer, non-blocking I/O (`O_NONBLOCK`), and `poll()` support, leaving COM1 console completely untouched.
- **Host Bridge (`tools/hostbridge.py`)**:
  - Host-side Python daemon bridging QEMU COM2 chardev to local/cloud models (`mock`, `anthropic`, `openai_compat`).
  - Strict host-side secret isolation: API keys are read solely on the host from environment variables and never transmitted to or stored on the guest.
  - Length-prefixed framing with CRC32 integrity verification and forward scanning for stream resynchronization over garbage bytes.
- **Token Budgets & Kernel Sycalls**:
  - `SYS_AGENT_CHARGE_TOKENS` (179) charges token usage against kernel agent budgets, returns `-EDQUOT` (132) on depletion, and logs `BUDGET_EXCEEDED` to the audit log.
  - `SYS_AGENT_TOKENS_LEFT` (180) allows agents and services to inspect remaining token allowances.
  - `SYS_MODEL_DAEMON_REGISTER` (181) restricts token charging privileges to the authorized model daemon.
  - `TokensUsed` tracked and exposed in `/proc/agents/<id>/budget`.
- **Model Backend Allowlist & Delegation Narrowing**:
  - `MODEL_BACKEND_LOCAL` (1) and `MODEL_BACKEND_HOST` (2) bitmasks stored in `agent_caps_t`, narrowing-only on delegation, visible in `/proc/agents/<id>/caps`, managed via `agentctl create --model-backend local,host`.
- **Local Model Inference Engine (`libllm`, `userland/llm/`)**:
  - llama2.c-style C transformer inference engine supporting INT8 quantized (`runq`) and FP32 checkpoints with greedy and temperature sampling.
  - Default tiny checkpoint `<2MB` (`/share/stories.bin`, `/share/tokenizer.bin`) loading seamlessly in <= 64MB RAM.
- **Model Daemon (`modeld`, `userland/modeld.c`)**:
  - Intent Bus provider daemon (`respawn` in `/etc/init.conf`) serving `model.complete`, `model.embed`, and `model.status`.
  - Automatic fallback (`backend: "auto"`) preferring host bridge when available and allowed, falling back to local `libllm`.
  - Audit logging with prompt SHA-256 hashing (never logging raw prompt/completion text).
- **Autonomous Agent Loop (`agentrun` & Lua `agent` module)**:
  - `agentrun <name> --goal "..."` CLI launcher attaching to a restricted kernel agent.
  - Dynamic tool discovery via `skill.list`, tool execution over Intent Bus, and prompt injection defense wrapping all tool outputs in `[UNTRUSTED DATA]` blocks.
  - Full execution trace persisted to `/home/out/agent-<id>.log`.
- **Verification Suite (`modeltest` & `tools/test_boot.sh` Session 18)**:
  - Automated validation of COM2 framing resilience, `model.status`/`model.embed`, host and local deterministic completions, token budget clamping & `-EDQUOT`, delegation narrowing, prompt injection defense, 100-cycle zero FD leaks, and SHA-256 audit chain verification.

---

## Stage 22 (done): AI-Native OS Phase 4 — The Agent Desktop: Accessibility Tree, Window Capabilities, Trusted Approval UI & Killswitch

Stage 22 delivers Phase 4 of the AI-Native Operating System: the **Agent Desktop**. Autonomous agents operate graphical user interface applications through a structured accessibility tree rather than raw pixels, protected by kernel-enforced window capabilities, visible control indicators, a compositor-level trusted approval UI, and an unblockable hardware emergency killswitch.

- **Structured Accessibility Tree (`libui`, `libwin`, `term`)**:
  - Every `libui` widget automatically exposes a semantic hierarchy: monotonic node ID, role (`window`, `button`, `textbox`, `listbox`, `dialog`, `terminal`, etc.), label/name, current value, screen bounds, state flags (`focused`, `enabled`, `checked`, etc.), and children without per-application code changes.
  - Secret masking: password and credential inputs mask their value as `"[REDACTED]"` unless the caller holds `CAP_WINDOW_SECRET`.
  - Terminal emulator (`term.c`) exposes role `terminal` and action `type_text` typing directly into the PTY master.
  - Callback-driven action dispatching: `click`, `set_text`, `select`, `toggle`, `focus` trigger native widget callbacks directly; never fake mouse clicks.
- **Window Ownership & Capabilities in `wm`**:
  - Display server authenticates clients using immutable kernel peer identity (`SYS_SOCK_PEERINFO`).
  - Agent capabilities: `CAP_WINDOW_OBSERVE` (read trees) and `CAP_WINDOW_ACT` (dispatch actions) enforced with per-application executable allowlists (`--window-observe files,uidemo --window-act files`).
  - Confidentiality flags: `CAP_WINDOW_SECRET` (read secret fields) and `CAP_WINDOW_RAW` (raw key/mouse injection and screendumps) default off and are never inherited.
  - Full audit logging: every window action emits `AUDIT_EVENT_WINDOW_ACTION` (14) with action name and hashed arguments.
- **UI Bridge Daemon (`uibridge`, `userland/uibridge.c`)**:
  - Intent Bus provider daemon (`respawn` in `/etc/init.conf`) registered as a privileged client to `wm`.
  - Skills provided: `ui.list_windows`, `ui.get_tree` (compact indented format optimized for LLM token budgets), `ui.act`, `ui.launch` (spawns apps inside caller's agent sandbox via `agent_attach`), `ui.wait`, `ui.focus_window`.
- **Compositor Trusted Approval UI (`/run/approval.sock`)**:
  - Modal approval card rendered directly into the compositor backbuffer for sensitive Intent Bus invocations.
  - Interactive choices: Deny (Esc), Allow Once (Enter), Allow Session with 30-second auto-deny timeout.
  - Headless fallback to CLI approval queue (`intentctl approve`/`deny`) when running in text mode.
- **Visible Control Indicators & Human Priority**:
  - Window borders dynamically colored based on active agent ID (`(agent_id - 1) % 6`).
  - Titlebars display `[Agent: <name>]` badge while an agent interacts with a window.
  - Target widget bounding box highlight for 500ms on `ui.act`.
  - Human-wins collision detection: hardware mouse/keyboard events within 500ms immediately abort conflicting in-flight agent actions.
- **Unblockable Human Killswitch**:
  - Intercepted directly inside PS/2 keyboard ISR (`src/keyboard.c`) via `Ctrl + Alt + Esc`.
  - Calls `agent_killswitch_toggle()` to suspend all active agents, halt window actions, and reject new agent tasks.
  - Compositor renders top high-visibility emergency banner: `[EMERGENCY STOP: ALL AGENTS SUSPENDED - Press Ctrl+Alt+Esc to resume]`.
  - Queryable/togglable via syscall `SYS_AGENT_KILLSWITCH_STATE` (182).
- **The Agent Dashboard (`agentpanel`, `userland/agentpanel.c`)**:
  - GUI desktop application featuring system header statistics, killswitch toggle button, preset launcher cards (`/etc/agents.d/*.conf`), running agents monitor, and Ask bar (`Super+Space` focus shortcut) communicating with `modeld`.
- **Verification Suite (`desktest`, `tools/test_step6.py`, `tools/test_boot.sh` Session 19)**:
  - Automated testing of killswitch transitions, audit logging, window capability narrowing, accessibility tree protocol round-tripping, `uibridge` bus skills, compositor emergency banner rendering with pixel-exact RGB assertions, and `-m 64` boot compatibility.

---

## Complete System Call Reference

MyOS implements a POSIX-compatible system call interface invoked via `int 0x80`:

| Syscall | Number | Signature / Description |
|---|---|---|
| `SYS_RESTART` | 0 | Internal syscall restart |
| `SYS_EXIT` | 1 | `void exit(int status)` -- terminate calling process |
| `SYS_FORK` | 2 | `pid_t fork(void)` -- clone address space with Copy-On-Write |
| `SYS_READ` | 3 | `ssize_t read(int fd, void *buf, size_t count)` |
| `SYS_WRITE` | 4 | `ssize_t write(int fd, const void *buf, size_t count)` |
| `SYS_OPEN` | 5 | `int open(const char *pathname, int flags, mode_t mode)` |
| `SYS_CLOSE` | 6 | `int close(int fd)` |
| `SYS_WAITPID` | 7 | `pid_t waitpid(pid_t pid, int *status, int options)` |
| `SYS_CREAT` | 8 | `int creat(const char *pathname, mode_t mode)` |
| `SYS_LINK` | 9 | `int link(const char *oldpath, const char *newpath)` |
| `SYS_UNLINK` | 10 | `int unlink(const char *pathname)` |
| `SYS_EXECVE` | 11 | `int execve(const char *pathname, char *const argv[], char *const envp[])` |
| `SYS_CHDIR` | 12 | `int chdir(const char *path)` |
| `SYS_TIME` | 13 | `time_t time(time_t *tloc)` |
| `SYS_LSEEK` | 19 | `off_t lseek(int fd, off_t offset, int whence)` |
| `SYS_GETPID` | 20 | `pid_t getpid(void)` |
| `SYS_SETUID` | 23 | `int setuid(uid_t uid)` |
| `SYS_GETUID` | 24 | `uid_t getuid(void)` |
| `SYS_PIPE` | 29 | `int pipe(int pipefd[2])` |
| `SYS_MMAP` | 30 | `void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)` |
| `SYS_MUNMAP` | 31 | `int munmap(void *addr, size_t length)` |
| `SYS_MPROTECT` | 32 | `int mprotect(void *addr, size_t len, int prot)` |
| `SYS_DUP` | 33 | `int dup(int oldfd)` |
| `SYS_DUP2` | 34 | `int dup2(int oldfd, int newfd)` |
| `SYS_FCNTL` | 35 | `int fcntl(int fd, int cmd, ...)` |
| `SYS_NANOSLEEP` | 36 | `int nanosleep(const struct timespec *req, struct timespec *rem)` |
| `SYS_CLOCK_GETTIME`| 37 | `int clock_gettime(clockid_t clk_id, struct timespec *tp)` |
| `SYS_SIGACTION` | 38 | `int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)` |
| `SYS_SIGPROCMASK` | 39 | `int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)` |
| `SYS_SIGRETURN` | 40 | `int sigreturn(void)` -- return from userspace signal trampoline |
| `SYS_KILL` | 41 | `int kill(pid_t pid, int sig)` |
| `SYS_ALARM` | 42 | `unsigned int alarm(unsigned int seconds)` |
| `SYS_PAUSE` | 43 | `int pause(void)` -- wait for signal interrupt |
| `SYS_SETPGID` | 44 | `int setpgid(pid_t pid, pid_t pgid)` |
| `SYS_GETPGRP` | 45 | `pid_t getpgrp(void)` |
| `SYS_SETSID` | 46 | `pid_t setsid(void)` |
| `SYS_SOCKET` | 47 | `int socket(int domain, int type, int protocol)` |
| `SYS_BIND` | 48 | `int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)` |
| `SYS_LISTEN` | 49 | `int listen(int sockfd, int backlog)` |
| `SYS_ACCEPT` | 50 | `int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)` |
| `SYS_CONNECT` | 51 | `int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)` |
| `SYS_SHUTDOWN` | 52 | `int shutdown(int sockfd, int how)` |
| `SYS_REBOOT` | 53 | `int reboot(int cmd)` -- poweroff, reboot, or halt with dirty cache sync |
| `SYS_TRUNCATE` | 54 | `int truncate(const char *path, off_t length)` |
| `SYS_FTRUNCATE` | 55 | `int ftruncate(int fd, off_t length)` |
| `SYS_IOCTL` | 56 | `int ioctl(int fd, unsigned long request, ...)` |
| `SYS_POLL` | 57 | `int poll(struct pollfd *fds, nfds_t nfds, int timeout)` |
| `SYS_SBRK` | 69 | `void *sbrk(intptr_t increment)` -- dynamic heap expansion |
| `SYS_GETDENTS` | 78 | `int getdents(unsigned int fd, struct dirent *dirp, unsigned int count)` |
| `SYS_STAT` | 106 | `int stat(const char *pathname, struct stat *statbuf)` |
| `SYS_FSTAT` | 108 | `int fstat(int fd, struct stat *statbuf)` |
| `SYS_AGENT_CREATE` | 170 | `int agent_create(const char *name, const agent_caps_t *caps, const agent_budget_t *budget)` |
| `SYS_AGENT_ATTACH` | 171 | `int agent_attach(int agent_id)` -- bind calling process to agent |
| `SYS_AGENT_KILL` | 172 | `int agent_kill(int agent_id)` -- kill agent and all its processes |
| `SYS_AGENT_SUSPEND` | 173 | `int agent_suspend(int agent_id)` -- suspend agent execution |
| `SYS_AGENT_RESUME` | 174 | `int agent_resume(int agent_id)` -- resume suspended agent |
| `SYS_AGENT_INFO` | 175 | `int agent_info(int agent_id, agent_info_t *info)` -- query agent status and metrics |
| `SYS_SOCK_PEERINFO`| 176 | `int sock_peerinfo(int fd, peerinfo_t *pinfo)` -- get peer pid and agent_id |
| `SYS_BUS_DAEMON_REGISTER`| 177 | `int bus_daemon_register(void)` -- register caller as privileged bus daemon |
| `SYS_AUDIT_APPEND` | 178 | `int audit_append(int agent_id, uint32_t event_type, const char *detail)` |
| `SYS_AGENT_CHARGE_TOKENS` | 179 | `int agent_charge_tokens(int agent_id, uint32_t n)` -- charge token usage to agent |
| `SYS_AGENT_TOKENS_LEFT`   | 180 | `int agent_tokens_left(int agent_id)` -- query remaining token budget |
| `SYS_MODEL_DAEMON_REGISTER`| 181 | `int model_daemon_register(void)` -- register caller as privileged model daemon |
| `SYS_AGENT_KILLSWITCH_STATE`| 182 | `int agent_killswitch_state(void)` / toggle (`ebx=1`) -- emergency agent killswitch |

---

## Verification Test Matrix

MyOS includes an automated test matrix executed via QEMU serial automation and monitor sockets:

| Suite / Session | Coverage | Key Assertions |
|---|---|---|
| **Session 1** | Boot, Shell, VFS, Core Utils | Command parsing, path resolution, `mkdir`, `cp`, `mv`, `rm`, `ls -l`, `fstest`, `lfntest`. |
| **Session 2** | Filesystem Persistence | Data written in Session 1 survives across QEMU reboot on the identical disk image. |
| **Session 3** | Pipes, Redirection, Pipelines | `cat \| grep \| wc`, multi-stage pipelines, `<` and `>`, background `&` execution. |
| **Session 4** | Host FAT Integrity Check | Host-side `dosfsck` / `fsck.fat -v` asserts zero cluster leaks or directory corruption. |
| **Session 5** | Low-Memory Boot (`-m 64`) | System boots and runs userspace successfully under constrained 64MB physical RAM. |
| **Session 6** | TTY & Signal Job Control | Interactive `Ctrl+C`, `Ctrl+Z`, `Ctrl+\`, `fg`, `bg`, `jobs`, `SIGTTIN`/`SIGTTOU` stopping. |
| **Session 7** | RTC Time, ProcFS & Wait Queues | CMOS UTC clock, non-busy `sleep`, `/proc` file enumeration, zero physical frame leaks. |
| **Session 8** | Newlib Libc & Lua 5.4 Runtime | Full `libctest` (stdio, math, malloc stress, setjmp), Lua scripts, table allocations. |
| **Session 9** | Framebuffer `/dev/fb0` & Input | Direct framebuffer `mmap()`, PS/2 mouse wheel stream, `/dev/input/*` events. |
| **Session 10** | Non-blocking I/O & Multiplexing | `poll()`, `select()`, `O_NONBLOCK` on pipes/TTYs, libc flag translation test. |
| **Session 10B** | Preemption Spinner Concurrency | 4 concurrent processes doing heavy disk operations alongside CPU spinners; `fsck.fat` clean. |
| **Session 11** | Graphical Demos & Crash Safety | `gfxdemo`, `paint`, `view`, `screenshot`, crash recovery from `KD_GRAPHICS` to `KD_TEXT`. |
| **Session 12** | QEMU Monitor Pixel Verification | Screendump framebuffer capture, RGB channel validation, synthetic input injection. |
| **Session 13** | Dirty Buffer Poweroff & Reboot | Poweroff and reboot flushing dirty ATA sectors before shutdown; zero corruption. |
| **Session 15** | Model-Based LFN Fuzzing | 500 pseudo-random filesystem operations; host `fsck.fat -v -a` and `mdir` validation. |
| **Session 16** | Kernel Agents & Security Subsystem | Default-deny caps, delegation narrowing, CPU/proc budgets, SHA-256 audit chain, zero frame leaks, `-m 64` boot. |
| **Session 17** | Intent Bus & Agent Skills | Daemon (`intentd`), `libjson`, `libintent`, peer identity, demo skills, approvals, `intentctl`, `-m 64` boot. |
| **Session 18** | Model Daemon & Agent Loop | COM2 raw framing resilience, `modeld`, token budgets & `-EDQUOT`, delegation narrowing, prompt injection defense, zero leaks, organizer demo, secret key isolation, `-m 64` boot. |
| **Session 19** | The Agent Desktop & Safety | Accessibility tree, window capabilities, `uibridge` Intent Bus skills, trusted approval UI, visible control indicators, unblockable hardware killswitch (`Ctrl+Alt+Esc`), `-m 64` boot. |
| **Step 1** | Window Protocol & `libwin` | AF_UNIX handshake, shared memory buffer attachments, damage reporting, `winhello`. |
| **Step 2** | Window Decorations & `wmctl` | Server-side titlebars, dragging, resizing, focus restacking, `wmctl` control commands. |
| **Step 3** | `libui` Widget Toolkit & `uidemo` | Complete widget tree validation (buttons, textboxes, textareas, listboxes, menus, dialogs). |
| **Step 4** | Graphical Terminal Emulator (`term`)| PTY VT100 / xterm-256color rendering, smooth scrolling, and shell integration. |
| **Step 5a** | Graphical File Manager (`files`) | Directory breadcrumb navigation, LFN rendering, item counters, and file operations. |

---

## Notes on the toolchain choices made here

- **Multiboot2 + GRUB** instead of a hand-rolled bootloader: lets us skip
  real-mode BIOS calls and disk driver code for now and start directly in
  protected mode. Writing your own bootloader is a great *later* exercise,
  not a prerequisite.
- **32-bit (i386), not 64-bit**: long mode requires setting up paging
  *before* the CPU can even execute long-mode code, which is extra
  complexity to front-load. Getting comfortable in protected mode first,
  then transitioning to long mode once paging is understood, is the more
  common learning path.



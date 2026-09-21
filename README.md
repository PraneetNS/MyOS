# MyOS — Stage 1

A minimal x86 kernel that boots via GRUB (Multiboot2), enters 32-bit
protected mode, and prints to the screen via a hand-written VGA text-mode
driver. This is the foundation stage of a "build your own OS" project.

## What's here

```
boot/
  boot.s       -- Multiboot2 header + assembly entry point (_start)
  linker.ld    -- linker script: places the kernel at the 1MB mark
  grub.cfg     -- tells GRUB how to boot myos.bin
src/
  kernel.c     -- kernel_main(), called from boot.s
  vga.c/.h     -- VGA text-mode driver (writes to 0xB8000)
Makefile
```

## Build & run (Linux, needs gcc, binutils, grub-pc-bin, xorriso, qemu)

```bash
sudo apt install build-essential grub-pc-bin grub-common xorriso qemu-system-x86

make         # builds boot/boot.o, src/*.o, links kernel, makes myos.iso
make run     # boots it in QEMU
```

`make run` uses `-serial stdio -display none`, so if you want to *see*
the VGA output (not just serial), instead run:

```bash
qemu-system-i386 -cdrom myos.iso
```

## What's actually happening on boot

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

## Stage 6 (next): process isolation

The single biggest thing missing now is memory protection between
processes. In order:

1. **Per-process page directories** — each process gets its own
   `CR3`-loadable page directory instead of sharing the kernel's.
2. **A proper `fork`/`exec`-style process model** — or at minimum,
   loading a *new* ELF while a previous one is still "running" (right
   now only one user program can be active at a time).
3. **User-mode heap** (`brk`/`sbrk`-style syscall) — programs currently
   get a fixed stack and nothing else.
4. **File writes** — the filesystem is read-only; adding writes means
   dealing with real allocation instead of "just append contiguously."
5. Eventually: replacing the kernel-stack-recursion trick from `sys_exit`
   with genuine process teardown back into a scheduler loop, unifying
   Stage 4's scheduler with Stage 5's process loading properly.

Resources: OSDev Wiki's "Higher Half Kernel" and "User Mode" pages cover
per-process address spaces; "Meaty Skeleton" covers the fork/exec model.

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

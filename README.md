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

## Stage 5 (next): a real filesystem and ELF loading

The ring-3 "process" right now is really just a function pointer baked
into the kernel binary at compile time. A real OS loads *actual programs*
from disk. In order:

1. **ATA/AHCI disk driver** — read raw sectors from a virtual disk.
2. **A filesystem** — start simple (FAT or even a custom flat format)
   before attempting something like ext2.
3. **ELF loader** — parse an ELF binary's program headers, map its
   segments into a process's address space, and jump to its entry point
   instead of a hardcoded kernel-side function pointer.
4. **Per-process address spaces** — right now every task shares the
   same page directory. Real process isolation needs each process to
   get its own page directory, with `paging.c`'s current "identity-map
   everything as user-accessible" simplification replaced by mapping
   only that process's own pages.
5. **A basic shell** that can load and run those programs on request —
   at that point this stops being a kernel with demos baked in and
   starts being an OS you actually *use*.

Resources: OSDev Wiki's "ATA PIO Mode", "FAT", and "ELF" pages, in
that order.

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

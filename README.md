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

## Stage 3 (next): paging and a heap allocator

This is the big one — real memory management. In order:

1. **Physical memory manager** — a bitmap or free-list tracking which
   4KB physical frames are in use, seeded from the Multiboot2 memory
   map GRUB hands you in `mb_info_addr` (currently unused — you'll
   parse it here).
2. **Paging** — build page tables, load `CR3`, set the paging bit in
   `CR0`. This is what gives you virtual memory and is a hard
   prerequisite for user-space processes later (Stage 4) and for
   eventually moving to 64-bit long mode.
3. **Page fault handler** — vector 14 already has a slot in the IDT;
   right now it just panics. Once paging is live, this is where you'll
   implement things like demand paging or a guard page for stack
   overflow detection.
4. **Kernel heap** — a `kmalloc`/`kfree` built on top of the physical
   allocator + paging, so the kernel itself can allocate memory
   dynamically instead of everything being static/global like it is now.

Resources: OSDev Wiki's "Memory Management" and "Paging" pages, and
the "Memory Management" chapters of OSTEP, cover this stage in the
same order.

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

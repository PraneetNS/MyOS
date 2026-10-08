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

### Known Limitations
1. **Signals (SIGPIPE, SIGCHLD, SIGINT)**:
   Signal delivery is deferred to a future stage. Writing to a pipe with no readers returns `-EPIPE` instead of sending `SIGPIPE`.
2. **Job Control**:
   Background jobs run asynchronously with `&`, but full POSIX job control (`Ctrl+Z`, `fg`, `bg`, terminal process groups) is not yet implemented.
3. **LFN (Long File Names)**:
   Filenames follow 8.3 FAT conventions; full VFAT LFN Unicode parsing is deferred.

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


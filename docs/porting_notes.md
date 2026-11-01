# MyOS 64-bit Porting Notes & Architectural Specifications

This document tracks all non-trivial architectural changes, ABI conventions, memory maps, and bugs resolved during the migration of MyOS from 32-bit x86 protected mode to 64-bit x86-64 long mode.

---

## 1. Virtual Memory & Canonical Address Space Layout

On x86-64 with 4-level paging (PML4 -> PDPT -> PD -> PT, 48-bit canonical addressing):
- **User Space**: Lower canonical half: `0x0000000000000000` to `0x00007FFFFFFFFFFF` (128 TB).
  - Executable base: `0x0000000000400000` (4 MB)
  - Heap: dynamic break via `sys_sbrk` growing upwards from `0x0000000010000000` (256 MB) or above `.bss`
  - User Stack: Base `0x00007FFFFFFFE000` (grows downward)
  - User MMAP region: `0x0000700000000000`
- **Direct Physical Map (PhysMap)**: `0xFFFF800000000000` to `0xFFFF8000FFFFFFFF` (covers all physical RAM up to 1TB using 1GB/2MB pages)
- **Kernel Space**: Higher canonical half (-2GB): `0xFFFFFFFF80000000` to `0xFFFFFFFFFFFFFFFF`
  - Kernel Code / Data / BSS: Loaded at physical 1MB (`0x100000`) and mapped to `0xFFFFFFFF80100000`
  - Kernel Heap (dynamic kheap): `0xFFFFFFFF90000000` to `0xFFFFFFFFB0000000` (512 MB)
  - Kernel MMIO (ioremap): `0xFFFFFFFFA0000000`

---

## 2. Boot Protocol: Multiboot2 32-bit Trampoline & Long Mode Transition

Because standard Multiboot2 loaders (like GRUB) jump to the 32-bit entry point in protected mode with paging disabled:
1. `_start` executes in 32-bit protected mode.
2. Sets up a temporary 4-level page table:
   - PML4 entry 0: Identity maps physical 0-1GB via 1GB page or 2MB pages.
   - PML4 entry 256 (`0xFFFF800000000000`): Direct physical map.
   - PML4 entry 511 (`0xFFFFFFFF80000000`): Maps kernel ELF to physical `0x00100000`.
3. Enables PAE in `CR4.PAE` (bit 5).
4. Enables Long Mode and NX in EFER MSR (`0xC0000080`): sets `EFER.LME` (bit 8) and `EFER.NXE` (bit 11).
5. Loads `CR3` with physical address of boot PML4.
6. Enables Paging (`CR0.PG`, bit 31).
7. Loads 64-bit GDT and executes a 64-bit far jump `ljmp $0x08, $long_mode_start` to enter 64-bit submode.
8. Unmaps lower identity page and enters `kernel_main`.

---

## 3. Syscall Convention & Register Mapping

- **Syscall Instruction**: `syscall` (fast path via `LSTAR` MSR `0xC0000082`, `STAR` `0xC0000081`, `SFMASK` `0xC0000084`).
- **Registers**:
  - `%rax`: Syscall Number
  - `%rdi`: Argument 1
  - `%rsi`: Argument 2
  - `%rdx`: Argument 3
  - `%r10`: Argument 4 (note: `syscall` instruction clobbers `%rcx` with `%rip` and `%r11` with `%rflags`)
  - `%r8` : Argument 5
  - `%r9` : Argument 6
  - `%rax`: Return code / negative errno
- **Backward Compatibility**: `int $0x80` IDT gate is also retained, forwarding parameters to the unified 64-bit syscall handler.

---

## 4. Modern CPU Hardening Features

- **NX (No-Execute)**: Enforced via `EFER.NXE` and bit 63 on all page table entries:
  - User stack, heap, and writable memory pages are explicitly marked NX.
  - Kernel text is read-only; kernel data and stack are NX (W^X strictly enforced).
- **SMEP (Supervisor Mode Execution Prevention)**: `CR4.SMEP` (bit 20) enabled when detected via `CPUID.(EAX=7, ECX=0):EBX[bit 7]`. Prevents the kernel from executing code in user-space pages.
- **SMAP (Supervisor Mode Access Prevention)**: `CR4.SMAP` (bit 21) enabled when detected via `CPUID.(EAX=7, ECX=0):EBX[bit 20]`. Prevents accidental kernel access to user memory; user access is strictly bracketed with `user_access_begin()` (`stac`) and `user_access_end()` (`clac`) during syscall execution.
- **ASLR**: Randomizes user base addresses (stack base, mmap base, heap brk) using entropy from the hardware/CSPRNG seed (`random_u32() & 0x1FFFFULL << 12`).

---

## 5. UEFI & BIOS Hybrid Boot Support

MyOS produces a hybrid bootable ISO (`myos.iso`) compatible with both legacy BIOS (Multiboot2 via GRUB) and modern UEFI (x86_64 GRUB-EFI via OVMF).
- **UEFI Boot Image**: The ISO filesystem embeds an EFI System Partition image (`/boot/efi.img` formatted FAT12) containing `EFI/BOOT/BOOTX64.EFI` generated with `grub-mkstandalone` (x86_64-efi format, modules: `part_gpt part_msdos fat ext2 iso9660 multiboot2 normal`).
- **El Torito Platform IDs**: `xorriso` configures El Torito boot catalogs for both BIOS (MBR/El Torito default boot catalog) and UEFI (`-eltorito-alt-boot -e boot/efi.img -no-emul-boot -isohybrid-gpt-basdat`).
- **Tested Environment**: Verified under QEMU with Tianocore OVMF (`-bios /usr/share/ovmf/OVMF.fd`).

---

## 6. Dynamic Physical Memory Management (>4GB RAM)

- **Scalable Frame Reference Tracking**: In 32-bit mode, `pmm_refcount` was a static array limited to 512MB RAM. In 64-bit mode, the PMM dynamically sizes the refcount array at boot based on the highest usable physical address reported by Multiboot2.
- **Placement**: Located immediately above the kernel image in early physical RAM, mapped into PhysMap (`0xFFFF800000000000`).
- **Scale Verification**: Tested and verified under QEMU with `-m 6144` (6GB, 1,572,735 frames) and `-m 8192` (8GB, 2,097,023 frames).

---

## 7. Resolved Architectural Caveats & Bugs

1. **SMAP Access in Syscall Dispatch**: Under `CR4.SMAP=1`, reading user-space arguments in `validate_user_string` or copying buffers to/from userspace faults unless `EFLAGS.AC=1`. Added `user_access_begin()` (`stac`) and `user_access_end()` (`clac`) around `syscall_dispatch()` and user memory probes.
2. **PhysMap Upper 32-Bit Carry**: In 32-bit boot trampoline assembly, mapping 64GB requires calculating upper 32-bits as `ecx >> 2` (`0x40000000` = 1GB per entry) to prevent 32-bit register overflow at 4GB.
3. **Red Zone Violation**: Kernel code compiled with `-mno-red-zone` prevents interrupt handlers from clobbering caller stack frames below `%rsp`. Userland code runs with standard System V AMD64 ABI.
4. **ELF String Literals vs Raw Disk Scratches**: Verification tests probing disk sectors for deleted text (e.g. `ROTATION123`) must not contain the target pattern as an ELF static string literal on disk; dynamically constructed strings at runtime prevent false-positive literal matches.


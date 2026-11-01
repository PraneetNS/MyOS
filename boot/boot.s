/* boot.s — Multiboot2 header + 32-bit to 64-bit Long Mode Trampoline + Higher-Half Entry */

.set MAGIC,      0xe85250d6      /* Multiboot2 magic number */
.set ARCH,       0               /* i386 protected mode */
.set HEADER_LEN, header_end - header_start
.set CHECKSUM,   -(MAGIC + ARCH + HEADER_LEN)

/* --- Multiboot2 header (must reside within first 32KB) --- */
.section .multiboot, "a"
.align 8
header_start:
    .long MAGIC
    .long ARCH
    .long HEADER_LEN
    .long CHECKSUM

    /* Framebuffer request tag (type 5) */
    .align 8
    .word 5    /* type: MULTIBOOT_HEADER_TAG_FRAMEBUFFER */
    .word 1    /* flags: optional */
    .long 20   /* size */
    .long 1024 /* width */
    .long 768  /* height */
    .long 32   /* depth */

    /* end tag */
    .align 8
    .word 0    /* type */
    .word 0    /* flags */
    .long 8    /* size */
header_end:

/* --- Boot Paging Structures (4KB aligned) in .boot_bss --- */
.section .boot_bss, "aw", @nobits
.align 4096
.global boot_pml4
boot_pml4:
    .skip 4096

.global boot_pdpt_identity
boot_pdpt_identity:
    .skip 4096

.global boot_pdpt_physmap
boot_pdpt_physmap:
    .skip 4096

.global boot_pdpt_kernel
boot_pdpt_kernel:
    .skip 4096

.global boot_pd_kernel
boot_pd_kernel:
    .skip 4096

/* Direct map 1GB pages PDs for up to 8GB */
.global boot_pd_physmap
boot_pd_physmap:
    .skip 4096 * 8

/* 64-bit GDT in boot_data */
.section .boot_data, "aw"
.align 16
gdt64:
    .quad 0x0000000000000000  /* 0x00: Null descriptor */
    .quad 0x00209A0000000000  /* 0x08: 64-bit Kernel Code (L=1, D=0, DPL=0, P=1, readable) */
    .quad 0x0000920000000000  /* 0x10: 64-bit Kernel Data (W=1, DPL=0, P=1) */
    .quad 0x0000FA0000000000  /* 0x18: 32-bit User Code (for compatibility) */
    .quad 0x0000F20000000000  /* 0x20: User Data (DPL=3, P=1) */
    .quad 0x0020FA0000000000  /* 0x28: 64-bit User Code (L=1, DPL=3, P=1) */
gdt64_desc:
    .word . - gdt64 - 1
    .long gdt64

/* --- 32-bit Boot Trampoline --- */
.section .boot_trampoline, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    cld

    /* Save multiboot info in ebx and magic in eax */
    mov %eax, %esi         /* esi = multiboot magic */
    mov %ebx, %edi         /* edi = multiboot info physical address */

    /* 1. Clear boot_pml4 (4096 bytes) */
    mov $boot_pml4, %eax
    mov %eax, %ebx         /* ebx = boot_pml4 physical address */
    xor %edx, %edx
    mov $1024, %ecx
2:
    mov %edx, (%eax)
    add $4, %eax
    loop 2b

    /* 2. Setup PML4 entries:
          PML4[0]   -> boot_pdpt_identity (maps low 1GB for smooth jump)
          PML4[256] -> boot_pdpt_physmap (maps 0xFFFF800000000000)
          PML4[511] -> boot_pdpt_kernel  (maps 0xFFFFFFFF80000000)
    */
    mov $boot_pdpt_identity, %eax
    or $0x03, %eax         /* Present | Write */
    mov %eax, 0(%ebx)      /* PML4[0] */

    mov $boot_pdpt_physmap, %eax
    or $0x03, %eax
    mov %eax, (256 * 8)(%ebx) /* PML4[256] */

    mov $boot_pdpt_kernel, %eax
    or $0x03, %eax
    mov %eax, (511 * 8)(%ebx) /* PML4[511] */

    /* 3. Setup boot_pdpt_identity:
          Map first 1GB using 1GB page (bit 7 PS=1):
          0x0000000000000000..0x000000003FFFFFFF -> phys 0..1GB
    */
    mov $boot_pdpt_identity, %eax
    movl $0x00000083, 0(%eax) /* 1GB page, Present | Write | PageSize */
    movl $0x00000000, 4(%eax)

    /* 4. Setup boot_pdpt_physmap (maps 64GB of RAM with 1GB pages):
          PDPT[0..63] -> 0..64GB physical
    */
    mov $boot_pdpt_physmap, %eax
    xor %ecx, %ecx
3:
    /* low 32 bits: ((ecx & 3) << 30) | 0x83 */
    mov %ecx, %edx
    and $3, %edx
    shl $30, %edx
    or $0x83, %edx
    mov %edx, (%eax)

    /* high 32 bits: ecx >> 2 */
    mov %ecx, %edx
    shr $2, %edx
    mov %edx, 4(%eax)

    add $8, %eax
    inc %ecx
    cmp $64, %ecx
    jne 3b

    /* 5. Setup boot_pdpt_kernel (maps -2GB):
          PML4[511] points here.
          Within PDPT[511], index 510 maps 0xFFFFFFFF80000000..0xFFFFFFFFBFFFFFFF (1GB).
          PDPT[510] points to boot_pd_kernel.
    */
    mov $boot_pdpt_kernel, %eax
    mov $boot_pd_kernel, %edx
    or $0x03, %edx
    mov %edx, (510 * 8)(%eax) /* PDPT[510] */

    /* 6. Setup boot_pd_kernel:
          Maps first 32MB of physical RAM to 0xFFFFFFFF80000000 using 2MB pages.
          PD[0..15] = 16 * 2MB = 32MB.
    */
    mov $boot_pd_kernel, %eax
    xor %ecx, %ecx
    mov $0x00000083, %edx  /* phys 0MB + Present | Write | PS (2MB) */
4:
    mov %edx, (%eax)
    movl $0, 4(%eax)
    add $0x00200000, %edx  /* +2MB */
    add $8, %eax
    inc %ecx
    cmp $16, %ecx
    jne 4b

    /* 7. Enable PAE in CR4 (bit 5) and PGE (bit 7) and OSFXSR/OSXMMEXCPT (bits 9, 10) */
    mov %cr4, %eax
    or $0x000006A0, %eax   /* PAE | PGE | OSFXSR | OSXMMEXCPT */
    mov %eax, %cr4

    /* 8. Load CR3 with PML4 physical address */
    mov %ebx, %cr3

    /* 9. Enable Long Mode (LME) and No-Execute (NXE) in EFER MSR (0xC0000080) */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x00000900, %eax   /* LME (bit 8) | NXE (bit 11) */
    wrmsr

    /* 10. Enable Paging (PG) and Protection (PE) in CR0 */
    mov %cr0, %eax
    or $0x80000001, %eax   /* PG (bit 31) | PE (bit 0) */
    mov %eax, %cr0

    /* 11. Load 64-bit GDT */
    mov $gdt64_desc, %eax
    lgdt (%eax)

    /* 12. Far jump to 64-bit mode */
    ljmp $0x08, $long_mode_entry

.code64
long_mode_entry:
    /* Reload data segment registers with 64-bit kernel data */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* Set 64-bit kernel stack in higher half */
    mov $stack_top, %rsp

    /* Jump to higher-half kernel entry point in .text */
    mov $higher_half_64, %rax
    jmp *%rax
.size _start, . - _start

/* --- 64-bit Long Mode Code in .text (Higher-Half) --- */
.section .text
.code64
.global higher_half_64
higher_half_64:
    /* Unmap identity map PML4[0] via PhysMap virtual address:
       PhysMap base is 0xFFFF800000000000 + phys_addr of boot_pml4 */
    mov $boot_pml4, %rax
    mov $0xFFFF800000000000, %rdx
    add %rdx, %rax
    movq $0, (%rax)

    /* Reload CR3 to flush TLB */
    mov %cr3, %rax
    mov %rax, %cr3

    /* Prepare multiboot2 arguments for kernel_main:
       rdi = magic (%rsi was saved)
       rsi = mb_info_addr (physical)
    */
    mov %rdi, %rax         /* rax = mb_info physical */
    mov %rsi, %rdi         /* rdi = magic (1st arg) */
    mov %rax, %rsi         /* rsi = mb_info physical (2nd arg) */

    /* Ensure 16-byte stack alignment before call */
    and $-16, %rsp

    call kernel_main

    /* Halt safely if kernel_main ever returns */
    cli
5:  hlt
    jmp 5b

/* --- Kernel Stack (32KB) in higher-half .bss --- */
.section .bss
.align 16
stack_bottom:
    .skip 32768
.global stack_top
stack_top:

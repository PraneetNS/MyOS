/* boot.s — Multiboot2 header + higher-half kernel entry point (32-bit protected mode) */

.set MAGIC,    0xe85250d6      /* Multiboot2 magic number */
.set ARCH,     0               /* i386 protected mode */
.set HEADER_LEN, header_end - header_start
.set CHECKSUM, -(MAGIC + ARCH + HEADER_LEN)

/* --- Multiboot2 header: GRUB scans the first 32KB for this --- */
.section .multiboot, "a"
.align 8
header_start:
    .long MAGIC
    .long ARCH
    .long HEADER_LEN
    .long CHECKSUM

    /* end tag */
    .align 8
    .word 0    /* type */
    .word 0    /* flags */
    .long 8    /* size */
header_end:

/* --- Page Directory (4KB aligned) in .bss --- */
.section .bss
.align 4096
.global boot_page_directory
boot_page_directory:
    .skip 4096

/* --- Stack (16KB, no execute) in .bss --- */
.align 16
stack_bottom:
    .skip 16384
stack_top:

/* --- Entry point --- */
.section .text
.global _start
.type _start, @function
_start:
    cld
    mov %eax, %esi
    mov %ebx, %ebp

    /* When GRUB jumps here, we are running at physical address ~0x100000.
       Paging is OFF. All symbols are linked at 0xC0100000+.
       We preserve %eax (multiboot magic) and %ebx (multiboot info phys addr). */

    /* 1. Clear boot_page_directory (1024 dwords = 4096 bytes) */
    mov $boot_page_directory, %edi
    sub $0xC0000000, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    /* 2. Set up identity map: PDE 0 and PDE 1 (0-8MB) as 4MB PSE pages */
    mov $boot_page_directory, %edi
    sub $0xC0000000, %edi
    movl $0x00000083, 0(%edi)       /* 0x00000000 - 0x003FFFFF (phys 0MB) */
    movl $0x00400083, 4(%edi)       /* 0x00400000 - 0x007FFFFF (phys 4MB) */

    /* 3. Set up direct map: 768MB (192 entries) starting at PDE 768 (0xC0000000) */
    /* PDE 768 is at byte offset 768 * 4 = 3072 */
    lea 3072(%edi), %edx
    mov $0x00000083, %eax
    mov $192, %ecx
1:
    mov %eax, (%edx)
    add $0x00400000, %eax
    add $4, %edx
    loop 1b

    /* 4. Enable 4MB PSE pages in CR4 (bit 4) */
    mov %cr4, %ecx
    or $0x00000010, %ecx
    mov %ecx, %cr4

    /* 5. Load CR3 with physical address of boot_page_directory */
    mov %edi, %cr3

    /* 6. Enable Paging in CR0 (bit 31: PG) */
    mov %cr0, %ecx
    or $0x80000000, %ecx
    mov %ecx, %cr0

    /* 7. Jump to higher-half code! */
    mov $higher_half, %ecx
    jmp *%ecx

higher_half:
    /* Now executing at virtual address in higher half (0xC010xxxx)! */
    mov $stack_top, %esp
    and $-16, %esp

    /* Restore multiboot parameters */
    mov %ebp, %ebx         /* multiboot info pointer (physical) */
    mov %esi, %eax         /* multiboot magic */

    test %ebx, %ebx
    jz 2f
    add $0xC0000000, %ebx  /* convert physical to direct map virtual address */
2:
    push %ebx              /* multiboot info pointer (virtual) */
    push %eax              /* multiboot magic value */

    call kernel_main

    /* kernel_main should never return, but halt safely if it does */
    cli
3:  hlt
    jmp 3b

.size _start, . - _start

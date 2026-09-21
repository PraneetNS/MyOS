/* boot.s — Multiboot2 header + kernel entry point (32-bit protected mode) */

.set MAGIC,    0xe85250d6      /* Multiboot2 magic number */
.set ARCH,     0               /* i386 protected mode */
.set HEADER_LEN, header_end - header_start
.set CHECKSUM, -(MAGIC + ARCH + HEADER_LEN)

/* --- Multiboot2 header: GRUB scans the first 32KB for this --- */
.section .multiboot
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

/* --- Stack (16KB, no execute) --- */
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

/* --- Entry point --- */
.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp   /* set up the stack pointer */
    and $-16, %esp         /* align stack to 16 bytes, per SysV ABI */

    push %ebx              /* multiboot info pointer (arg for kernel_main) */
    push %eax              /* multiboot magic value   */

    call kernel_main

    /* kernel_main should never return, but halt safely if it does */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

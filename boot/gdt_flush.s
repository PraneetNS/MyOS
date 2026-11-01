.section .text
.code64
.global gdt_flush
.type gdt_flush, @function
/* void gdt_flush(uint64_t gdt_ptr_addr)
   rdi = pointer to gdt_ptr struct:
         limit: uint16_t (2 bytes)
         base:  uint64_t (8 bytes)
*/
gdt_flush:
    lgdt (%rdi)

    mov $0x10, %ax      /* 0x10 = 64-bit kernel data segment selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Far return to reload CS with 64-bit kernel code selector (0x08) */
    pushq $0x08
    leaq .flush(%rip), %rax
    pushq %rax
    lretq
.flush:
    ret
.size gdt_flush, . - gdt_flush

.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    mov 4(%esp), %eax   /* arg: pointer to gdt_ptr struct */
    lgdt (%eax)

    mov $0x10, %ax      /* 0x10 = kernel data segment selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Far jump to reload CS with the new kernel code selector (0x08) */
    ljmp $0x08, $.flush
.flush:
    ret
.size gdt_flush, . - gdt_flush

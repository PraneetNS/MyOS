.section .text
.code64
.global idt_flush
.type idt_flush, @function
/* void idt_flush(uint64_t idt_ptr_addr)
   rdi = pointer to idt_ptr struct (limit: uint16_t, base: uint64_t)
*/
idt_flush:
    lidt (%rdi)
    ret
.size idt_flush, . - idt_flush

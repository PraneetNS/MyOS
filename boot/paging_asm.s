.section .text

.global paging_flush
.type paging_flush, @function
paging_flush:
    mov 4(%esp), %eax
    mov %eax, %cr3
    ret
.size paging_flush, . - paging_flush

.global paging_enable
.type paging_enable, @function
paging_enable:
    mov %cr0, %eax
    or $0x80000000, %eax   /* PG bit (bit 31) */
    mov %eax, %cr0
    ret
.size paging_enable, . - paging_enable

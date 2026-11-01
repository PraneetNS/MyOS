.section .text
.code64

.global paging_flush
.type paging_flush, @function
/* void paging_flush(uint64_t pml4_phys) */
paging_flush:
    mov %rdi, %cr3
    ret
.size paging_flush, . - paging_flush

.global invlpg_page
.type invlpg_page, @function
/* void invlpg_page(uint64_t vaddr) */
invlpg_page:
    invlpg (%rdi)
    ret
.size invlpg_page, . - invlpg_page

.section .text
.code64

.global copy_to_user
.global copy_from_user
.global copy_user_start
.global copy_user_end
.global copy_user_start2
.global copy_user_end2
.global copy_user_fault

/* int copy_to_user(void* user_dest, const void* kernel_src, uint64_t len)
   rdi = user_dest
   rsi = kernel_src
   rdx = len
*/
copy_to_user:
    pushfq
    test %rdx, %rdx
    jz 1f

    /* Bounds check user_dest range: [0x1000 .. 0x0000800000000000) */
    mov %rdi, %rax
    cmp $0x1000, %rax
    jb copy_user_fault
    add %rdx, %rax
    jc copy_user_fault
    mov $0x0000800000000000, %rcx
    cmp %rcx, %rax
    ja copy_user_fault

    mov %rdx, %rcx

    /* SMAP: open access to user pages if enabled */
    cmpb $0, smap_enabled(%rip)
    je copy_user_start
    stac
copy_user_start:
    rep movsb
copy_user_end:
1:
    popfq
    xor %eax, %eax
    ret

/* int copy_from_user(void* kernel_dest, const void* user_src, uint64_t len)
   rdi = kernel_dest
   rsi = user_src
   rdx = len
*/
copy_from_user:
    pushfq
    test %rdx, %rdx
    jz 2f

    /* Bounds check user_src range */
    mov %rsi, %rax
    cmp $0x1000, %rax
    jb copy_user_fault
    add %rdx, %rax
    jc copy_user_fault
    mov $0x0000800000000000, %rcx
    cmp %rcx, %rax
    ja copy_user_fault

    mov %rdx, %rcx

    /* SMAP: open access to user pages if enabled */
    cmpb $0, smap_enabled(%rip)
    je copy_user_start2
    stac
copy_user_start2:
    rep movsb
copy_user_end2:
2:
    popfq
    xor %eax, %eax
    ret

copy_user_fault:
    popfq
    mov $-14, %eax      /* -EFAULT (-14) */
    ret

.section .note.GNU-stack,"",@progbits

.global copy_to_user
.global copy_from_user
.global copy_user_start
.global copy_user_end
.global copy_user_start2
.global copy_user_end2
.global copy_user_fault

.section .text

# int copy_to_user(void* user_dest, const void* kernel_src, uint32_t len)
copy_to_user:
    push %ebp
    mov %esp, %ebp
    push %esi
    push %edi
    push %ecx

    mov 8(%ebp), %edi     # user_dest
    mov 12(%ebp), %esi    # kernel_src
    mov 16(%ebp), %ecx    # len

    test %ecx, %ecx
    jz 1f

    # Bounds check user_dest range [user_dest, user_dest + len)
    cmp $0x1000, %edi
    jb copy_user_fault
    mov %edi, %eax
    add %ecx, %eax
    jc copy_user_fault
    cmp $0xC0000000, %eax
    ja copy_user_fault

copy_user_start:
    rep movsb
copy_user_end:
1:
    xor %eax, %eax        # 0 = success
    pop %ecx
    pop %edi
    pop %esi
    mov %ebp, %esp
    pop %ebp
    ret

# int copy_from_user(void* kernel_dest, const void* user_src, uint32_t len)
copy_from_user:
    push %ebp
    mov %esp, %ebp
    push %esi
    push %edi
    push %ecx

    mov 8(%ebp), %edi     # kernel_dest
    mov 12(%ebp), %esi    # user_src
    mov 16(%ebp), %ecx    # len

    test %ecx, %ecx
    jz 2f

    # Bounds check user_src range [user_src, user_src + len)
    cmp $0x1000, %esi
    jb copy_user_fault
    mov %esi, %eax
    add %ecx, %eax
    jc copy_user_fault
    cmp $0xC0000000, %eax
    ja copy_user_fault

copy_user_start2:
    rep movsb
copy_user_end2:
2:
    xor %eax, %eax        # 0 = success
    pop %ecx
    pop %edi
    pop %esi
    mov %ebp, %esp
    pop %ebp
    ret

copy_user_fault:
    mov $-14, %eax        # -EFAULT (-14)
    pop %ecx
    pop %edi
    pop %esi
    mov %ebp, %esp
    pop %ebp
    ret

.section .note.GNU-stack,"",@progbits

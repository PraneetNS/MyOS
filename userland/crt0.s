.global environ
.data
environ:
    .quad 0

.section .text
.global _start
.type _start, @function

_start:
    xorq %rbp, %rbp

    popq %rdi             /* argc */
    movq %rsp, %rsi       /* argv */

    /* Compute envp: argv + 8*(argc + 1) */
    movq %rdi, %rax
    incq %rax
    shlq $3, %rax
    addq %rsp, %rax       /* envp */
    movq %rax, %rdx
    movq %rax, environ(%rip)

    andq $-16, %rsp

    call main

    movl %eax, %edi
    movq $0, %rax         /* SYS_EXIT */
    syscall
    int $0x80

1:  hlt
    jmp 1b

.size _start, . - _start

.global __sigreturn_restorer
.type __sigreturn_restorer, @function
__sigreturn_restorer:
    movq $15, %rax        /* Linux x86-64 __NR_rt_sigreturn */
    syscall
    int $0x80
.size __sigreturn_restorer, . - __sigreturn_restorer

.section .note.GNU-stack,"",@progbits


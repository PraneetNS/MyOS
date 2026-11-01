.section .data
.align 8
user_rsp_scratch:
    .quad 0

.section .text
.code64
.global syscall_entry
.extern tss
.extern syscall_dispatch

syscall_entry:
    /* Save user RSP */
    movq %rsp, user_rsp_scratch(%rip)

    /* Switch to kernel stack from TSS (tss.rsp0 is at offset 4 of struct tss_entry64) */
    movq tss+4(%rip), %rsp

    /* Construct struct registers frame on kernel stack */
    pushq $0x23                  /* ss: User Data selector (0x20 | 3) */
    pushq user_rsp_scratch(%rip) /* rsp: saved user stack */
    pushq %r11                   /* rflags: saved by syscall in r11 */
    pushq $0x2B                  /* cs: User Code 64 selector (0x28 | 3) */
    pushq %rcx                   /* rip: saved by syscall in rcx */
    pushq $0                     /* err_code */
    pushq $0x82                  /* int_no: 0x82 denotes fast syscall */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %ds, %rax
    pushq %rax

    /* Pass pointer to struct registers as 1st argument */
    movq %rsp, %rdi

    /* 16-byte stack alignment for C ABI */
    movq %rsp, %rbp
    andq $-16, %rsp

    cld
    call syscall_dispatch

    movq %rbp, %rsp

    /* Restore segment */
    popq %rax
    movq %rax, %ds
    movq %rax, %es

    /* Restore general purpose registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rsi
    popq %rdi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Pop int_no and err_code */
    addq $16, %rsp

    iretq
.size syscall_entry, . - syscall_entry


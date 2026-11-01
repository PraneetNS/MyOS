.section .text
.code64

/* Macro for exceptions that DON'T push an error code (dummy 0 pushed) */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    pushq $0                /* dummy error code */
    pushq $\num             /* interrupt vector number */
    jmp isr_common_stub
.endm

/* Macro for exceptions that DO push an error code automatically */
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    pushq $\num             /* interrupt vector number */
    jmp isr_common_stub
.endm

.macro IRQ num, int_no
.global irq\num
irq\num:
    pushq $0                /* dummy error code */
    pushq $\int_no          /* IRQ vector number */
    jmp irq_common_stub
.endm

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

/* IRQs 0..15 mapped to vectors 32..47 */
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* System Call Gate (int $0x80) */
.global isr128
isr128:
    pushq $0
    pushq $128
    jmp isr_common_stub

.extern isr_handler
.extern irq_handler

isr_common_stub:
    /* Push all general purpose registers to match struct registers */
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

    /* Push data segment */
    movq %ds, %rax
    pushq %rax

    /* Pass pointer to struct registers as 1st argument (%rdi) */
    movq %rsp, %rdi

    /* Ensure 16-byte stack alignment before calling C */
    movq %rsp, %rbp
    andq $-16, %rsp

    cld
    call isr_handler

    movq %rbp, %rsp

    /* Pop data segment */
    popq %rax
    movq %rax, %ds
    movq %rax, %es

    /* Pop general purpose registers */
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

irq_common_stub:
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

    movq %rsp, %rdi

    movq %rsp, %rbp
    andq $-16, %rsp

    cld
    call irq_handler

    movq %rbp, %rsp

    popq %rax
    movq %rax, %ds
    movq %rax, %es

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

    addq $16, %rsp

    iretq

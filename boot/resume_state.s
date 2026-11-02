.section .text
.code64

.global resume_saved_state
.type resume_saved_state, @function
/* void resume_saved_state(struct registers* r) -- never returns
   rdi = pointer to struct registers
*/
resume_saved_state:
    /* Load data segment registers (leave %fs and %gs intact to preserve MSR_FS_BASE / TLS) */
    mov 0(%rdi), %rax
    mov %ax, %ds
    mov %ax, %es

    /* Push iretq frame:
       SS     = r->ss     (offset 176)
       RSP    = r->rsp    (offset 168)
       RFLAGS = r->rflags (offset 160)
       CS     = r->cs     (offset 152)
       RIP    = r->rip    (offset 144)
    */
    pushq 176(%rdi)   /* ss */
    pushq 168(%rdi)   /* rsp */
    pushq 160(%rdi)   /* rflags */
    pushq 152(%rdi)   /* cs */
    pushq 144(%rdi)   /* rip */

    /* Restore general purpose registers */
    movq 8(%rdi),   %r15
    movq 16(%rdi),  %r14
    movq 24(%rdi),  %r13
    movq 32(%rdi),  %r12
    movq 40(%rdi),  %r11
    movq 48(%rdi),  %r10
    movq 56(%rdi),  %r9
    movq 64(%rdi),  %r8
    movq 72(%rdi),  %rbp
    movq 80(%rdi),  %rsi
    movq 96(%rdi),  %rdx
    movq 104(%rdi), %rcx
    movq 112(%rdi), %rbx
    movq 120(%rdi), %rax

    /* Restore rdi last */
    movq 88(%rdi),  %rdi

    iretq
.size resume_saved_state, . - resume_saved_state

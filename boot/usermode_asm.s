.section .text
.code64

.global enter_usermode
.type enter_usermode, @function
/* void enter_usermode(uint64_t entry_point, uint64_t user_stack_top)
   rdi = entry_point
   rsi = user_stack_top
*/
enter_usermode:
    cli

    /* User data segment: 0x20 | RPL 3 = 0x23 */
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Push 64-bit iretq frame:
       [rsp + 32] = SS     (0x23)
       [rsp + 24] = RSP    (user_stack_top)
       [rsp + 16] = RFLAGS (IF enabled = 0x202)
       [rsp + 8]  = CS     (0x28 | 3 = 0x2B)
       [rsp]      = RIP    (entry_point)
    */
    pushq $0x23            /* SS */
    pushq %rsi             /* RSP */
    pushq $0x202           /* RFLAGS (IF=1) */
    pushq $0x2B            /* CS (64-bit User Code Selector) */
    pushq %rdi            /* RIP */

    /* Clear all general-purpose registers before entering userland */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15

    iretq
.size enter_usermode, . - enter_usermode

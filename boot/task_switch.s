.section .text
.code64

.global switch_task
.type switch_task, @function
/* void switch_task(uint64_t* old_rsp_store, uint64_t new_rsp)
   rdi = old_rsp_store
   rsi = new_rsp
*/
switch_task:
    /* Push callee-saved registers on old stack */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq

    /* Save old rsp */
    movq %rsp, (%rdi)

    /* Switch to new rsp */
    movq %rsi, %rsp

    /* Pop callee-saved registers from new stack */
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size switch_task, . - switch_task

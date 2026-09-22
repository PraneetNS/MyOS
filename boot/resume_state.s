.section .text
.global resume_saved_state
.type resume_saved_state, @function
/* void resume_saved_state(struct registers* r) -- never returns.
   Field offsets must match struct registers in src/idt.h exactly:
   ds=0, edi=4, esi=8, ebp=12, esp=16(unused, matches POPA's own
   behavior of not restoring esp), ebx=20, edx=24, ecx=28, eax=32,
   int_no=36, err_code=40, eip=44, cs=48, eflags=52, useresp=56, ss=60. */
resume_saved_state:
    mov 4(%esp), %ebp    /* ebp = r, temporarily repurposed as a base pointer */

    mov 0(%ebp), %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Build the iret frame on the CURRENT (kernel) stack: push in
       reverse order so iret pops eip, cs, eflags, useresp, ss. */
    push 60(%ebp)   /* ss */
    push 56(%ebp)   /* useresp */
    push 52(%ebp)   /* eflags */
    push 48(%ebp)   /* cs */
    push 44(%ebp)   /* eip */

    mov 20(%ebp), %ebx
    mov 24(%ebp), %edx
    mov 28(%ebp), %ecx
    mov 4(%ebp),  %edi
    mov 8(%ebp),  %esi
    mov 32(%ebp), %eax   /* r->eax -- the caller sets this to 0 for the child, matching fork() semantics */

    mov 12(%ebp), %ebp   /* load the real saved ebp LAST -- it's our base pointer until this instruction */

    iret
.size resume_saved_state, . - resume_saved_state

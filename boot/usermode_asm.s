.section .text
.global enter_usermode
.type enter_usermode, @function
enter_usermode:
    mov 4(%esp), %eax    /* entry_point */
    mov 8(%esp), %ecx    /* user_stack_top */

    cli

    mov $0x23, %bx        /* user data selector (0x20 | RPL 3) */
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs
                           /* ss is NOT loaded here -- iret loads it from the stack below */

    push $0x23             /* user ss */
    push %ecx               /* user esp */

    pushf
    pop %edx
    or $0x200, %edx        /* set IF so interrupts are enabled once we're in user mode */
    push %edx               /* eflags */

    push $0x1B             /* user cs (0x18 | RPL 3) */
    push %eax                /* user eip = entry_point */

    iret
.size enter_usermode, . - enter_usermode

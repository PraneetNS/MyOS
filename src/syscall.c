#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"

#define SYS_EXIT  0
#define SYS_WRITE 1

extern void isr128(void); /* defined in isr.s */

static void syscall_handler(struct registers* regs) {
    switch (regs->eax) {
        case SYS_WRITE:
            /* regs->ebx = pointer to a NUL-terminated string, set by the
               caller in EBX before `int 0x80` (see userland/hello.c) */
            terminal_writestring((const char*) regs->ebx);
            break;

        case SYS_EXIT:
            terminal_writestring("[ok] process exited\n");
            /* Tears the process down and switch_task()s into whatever's
               next in the round-robin rotation. Because this goes
               through switch_task's popf (not a bare function call),
               interrupts get correctly re-enabled for whatever we
               resume -- no manual `sti` needed here, unlike Stage 5/6's
               shell_run()-recursion design this replaces. */
            scheduler_exit_current(); /* never returns */
            break;

        default:
            terminal_writestring("[warn] unknown syscall\n");
            break;
    }
}

void syscall_install(void) {
    /* 0xEE = present, DPL=3 (so ring-3 code is allowed to `int 0x80`
       without a general protection fault), 32-bit interrupt gate */
    idt_set_gate(128, (uint32_t) isr128, 0x08, 0xEE);
    register_interrupt_handler(128, &syscall_handler);
}

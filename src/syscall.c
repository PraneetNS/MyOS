#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "shell.h"
#include "vmm.h"

#define SYS_EXIT  0
#define SYS_WRITE 1

extern void isr128(void); /* defined in isr.s */

static void syscall_handler(struct registers* regs) {
    switch (regs->eax) {
        case SYS_WRITE:
            /* regs->ebx = pointer to a NUL-terminated string, set by the
               caller in EBX before `int 0x80` (see usermode_demo.c) */
            terminal_writestring((const char*) regs->ebx);
            break;

        case SYS_EXIT:
            terminal_writestring("[ok] program exited\n");
            vmm_switch_to_kernel(); /* leave the process's address space, reclaim isolation */
            /* We're about to call shell_run(), which never returns --
               that means we permanently skip this handler's own normal
               epilogue (in isr_common_stub), which is where `sti` would
               otherwise re-enable interrupts. Without this explicit
               sti, the keyboard (and everything else) would silently
               stop firing forever after the first program exits. */
            asm volatile ("sti");
            /* NOTE: this re-enters the shell via a fresh nested call
               rather than a true "return" to where it was launched from
               -- see the README's Stage 5 notes on why, and its
               documented limitation (unbounded kernel stack growth
               across many "run" commands in one session). A real kernel
               would instead tear down the process and hand control back
               to a scheduler, as Stage 4's demo did. */
            shell_run(); /* never returns */
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

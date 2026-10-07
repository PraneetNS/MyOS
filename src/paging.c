#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "serial.h"

extern void paging_flush(uint32_t page_directory_phys);

static void page_fault_handler(struct registers* regs) {
    uint32_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    int from_usermode = (regs->cs & 0x3) == 3; /* RPL bits of the faulting CS */

    kprintf("\n*** PAGE FAULT at 0x%08x (err=0x%08x, %s, %s, eip=0x%08x) ***\n",
            faulting_addr, regs->err_code,
            (regs->err_code & 0x4) ? "user-mode" : "kernel-mode",
            (regs->err_code & 0x2) ? "write" : "read",
            regs->eip);

    if (from_usermode) {
        kprintf("Process terminated (illegal memory access).\n");
        scheduler_exit_current(139); /* never returns */
    }

    kprintf("Kernel-mode page fault -- this is a real kernel bug. System halted.\n");
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

void paging_init(void) {
    /* Drop the temporary boot identity map (PDE 0 and PDE 1) */
    boot_page_directory[0] = 0;
    boot_page_directory[1] = 0;

    /* Flush TLB by reloading CR3 with kernel page directory physical address */
    paging_flush(paging_get_kernel_dir_phys());

    register_interrupt_handler(14, &page_fault_handler); /* vector 14 = #PF */

    terminal_writestring("[ok] Paging initialized (higher-half direct map active, identity dropped)\n");
}

uint32_t paging_get_kernel_dir_phys(void) {
    return V2P(&boot_page_directory);
}

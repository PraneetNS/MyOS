#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "serial.h"
#include "vma.h"
#include "process.h"
#include "uaccess.h"

extern void paging_flush(uint32_t page_directory_phys);

static void page_fault_handler(struct registers* regs) {
    uint32_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    int from_usermode = (regs->cs & 0x3) == 3; /* RPL bits of the faulting CS */
    uint32_t err = regs->err_code;
    process_t* proc = scheduler_current();

    kprintf("\n*** PAGE FAULT at 0x%08x (err=0x%08x, %s, %s, eip=0x%08x) ***\n",
            faulting_addr, regs->err_code,
            (regs->err_code & 0x4) ? "user-mode" : "kernel-mode",
            (regs->err_code & 0x2) ? "write" : "read",
            regs->eip);

    int in_uaccess = ((uint32_t)regs->eip >= (uint32_t)copy_user_start && (uint32_t)regs->eip < (uint32_t)copy_user_end) ||
                     ((uint32_t)regs->eip >= (uint32_t)copy_user_start2 && (uint32_t)regs->eip < (uint32_t)copy_user_end2);

    /* Check if this is a user-space address */
    if (faulting_addr >= 0x1000 && faulting_addr < 0xC0000000 && proc && !proc->is_kernel_task) {
        vma_t* vma = vma_find(proc, faulting_addr);
        if (vma) {
            int is_write = (err & 0x2) != 0;
            if (is_write && !(vma->prot & VMA_PROT_WRITE)) {
                /* Protection violation: write to read-only VMA */
                if (from_usermode) {
                    serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): write protection violation in VMA [0x%08x-0x%08x prot=0x%x]\n",
                                  proc->pid, proc->name, faulting_addr, regs->eip, err, vma->start, vma->end, vma->prot);
                    kprintf("Process %d terminated (protection violation at 0x%08x).\n", proc->pid, faulting_addr);
                    scheduler_exit_current(139); /* never returns */
                }
                if (in_uaccess) {
                    regs->eip = (uint32_t) copy_user_fault;
                    return;
                }
            } else if (!is_write && (err & 0x1)) {
                /* Protection violation: read/exec protection violation */
                if (from_usermode) {
                    serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): protection violation in VMA [0x%08x-0x%08x prot=0x%x]\n",
                                  proc->pid, proc->name, faulting_addr, regs->eip, err, vma->start, vma->end, vma->prot);
                    kprintf("Process %d terminated (protection violation at 0x%08x).\n", proc->pid, faulting_addr);
                    scheduler_exit_current(139);
                }
                if (in_uaccess) {
                    regs->eip = (uint32_t) copy_user_fault;
                    return;
                }
            } else {
                /* Page not present inside valid VMA - handled by demand paging in Step 5 */
            }
        } else {
            /* Fault outside any VMA */
            if (from_usermode) {
                serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): outside any VMA\n",
                              proc->pid, proc->name, faulting_addr, regs->eip, err);
                kprintf("Process %d terminated (page fault at 0x%08x, outside any VMA).\n", proc->pid, faulting_addr);
                scheduler_exit_current(139);
            }
            if (in_uaccess) {
                regs->eip = (uint32_t) copy_user_fault;
                return;
            }
        }
    }

    if (from_usermode) {
        serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): illegal user access\n",
                      proc ? proc->pid : -1, proc ? proc->name : "?", faulting_addr, regs->eip, err);
        kprintf("Process terminated (page fault at 0x%08x).\n", faulting_addr);
        scheduler_exit_current(139);
    }

    if (in_uaccess) {
        regs->eip = (uint32_t) copy_user_fault;
        return;
    }

    kprintf("\n*** KERNEL PAGE FAULT at 0x%08x (err=0x%08x, eip=0x%08x) ***\n",
            faulting_addr, regs->err_code, regs->eip);
    serial_printf("*** KERNEL PAGE FAULT at 0x%08x (err=0x%08x, eip=0x%08x) ***\n",
                  faulting_addr, regs->err_code, regs->eip);
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

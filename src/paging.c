#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "serial.h"
#include "vma.h"
#include "process.h"
#include "uaccess.h"
#include "pmm.h"
#include "vmm.h"
#include "vfs.h"

extern void paging_flush(uint32_t page_directory_phys);

static void page_fault_handler(struct registers* regs) {
    uint32_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    int from_usermode = (regs->cs & 0x3) == 3; /* RPL bits of the faulting CS */
    uint32_t err = regs->err_code;
    process_t* proc = scheduler_current();

    int in_uaccess = ((uint32_t)regs->eip >= (uint32_t)copy_user_start && (uint32_t)regs->eip < (uint32_t)copy_user_end) ||
                     ((uint32_t)regs->eip >= (uint32_t)copy_user_start2 && (uint32_t)regs->eip < (uint32_t)copy_user_end2);

    /* 1. Demand Paging & Stack Growth: only if page was not present */
    if (!(err & 1) && proc && !proc->is_kernel_task) {
        /* Check stack auto-growth: [USER_STACK_BOTTOM, USER_STACK_TOP) */
        if (faulting_addr >= USER_STACK_BOTTOM && faulting_addr < USER_STACK_TOP) {
            vma_t* v = proc->vma_list;
            while (v) {
                if (v->flags & VMA_FLAG_STACK) {
                    if (faulting_addr < v->start) {
                        v->start = faulting_addr & ~0xFFFu;
                    }
                    break;
                }
                v = v->next;
            }
        }

        vma_t* vma = vma_find(proc, faulting_addr);
        if (vma) {
            int is_write = (err & 0x2) != 0;
            if (is_write && !(vma->prot & VMA_PROT_WRITE)) {
                /* Write to non-writable VMA -> protection violation */
            } else if (!is_write && !(vma->prot & (VMA_PROT_READ | VMA_PROT_EXEC))) {
                /* Read from unreadable VMA -> protection violation */
            } else {
                /* Valid demand-page fault! Allocate physical frame */
                uint32_t frame_phys = pmm_alloc_frame();
                if (!frame_phys) {
                    serial_printf("[pf] OOM: cannot allocate frame for PID %d\n", proc->pid);
                    scheduler_exit_current(139);
                }

                uint32_t page_va = faulting_addr & ~0xFFFu;
                uint8_t* page_ptr = (uint8_t*) P2V(frame_phys);
                for (int i = 0; i < 4096; i++) page_ptr[i] = 0;

                /* If file-backed, load data from file */
                if ((vma->flags & VMA_FLAG_FILE) && vma->file) {
                    if (page_va < vma->start + vma->file_size) {
                        uint32_t off_in_vma = page_va - vma->start;
                        uint32_t file_offset = vma->offset + off_in_vma;
                        uint32_t to_read = 4096;
                        if (off_in_vma + to_read > vma->file_size) {
                            to_read = vma->file_size - off_in_vma;
                        }
                        if (vma->file->ops && vma->file->ops->read) {
                            vma->file->ops->read(vma->file, file_offset, page_ptr, to_read);
                        }
                    }
                }

                uint32_t pte_flags = PAGE_PRESENT | PAGE_USER;
                if (vma->prot & VMA_PROT_WRITE) pte_flags |= PAGE_WRITE;

                if (vmm_map_page(&proc->as, page_va, frame_phys, pte_flags) != 0) {
                    pmm_unref(frame_phys);
                    scheduler_exit_current(139);
                }

                asm volatile ("invlpg (%0)" :: "r"(page_va) : "memory");
                return; /* Successfully paged in! */
            }
        }
    }

    /* 2. Unhandled / Fatal page fault: log and terminate process */
    kprintf("\n*** PAGE FAULT at 0x%08x (err=0x%08x, %s, %s, eip=0x%08x) ***\n",
            faulting_addr, regs->err_code,
            (regs->err_code & 0x4) ? "user-mode" : "kernel-mode",
            (regs->err_code & 0x2) ? "write" : "read",
            regs->eip);

    if (proc && !proc->is_kernel_task) {
        vma_t* vma = vma_find(proc, faulting_addr);
        if (!vma) {
            serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): outside any VMA\n",
                          proc->pid, proc->name, faulting_addr, regs->eip, err);
            kprintf("Process %d terminated (page fault at 0x%08x, outside any VMA).\n", proc->pid, faulting_addr);
        } else {
            serial_printf("[pf] PID %d (%s) page fault at 0x%08x (eip=0x%08x, err=0x%x): protection violation in VMA [0x%08x-0x%08x prot=0x%x]\n",
                          proc->pid, proc->name, faulting_addr, regs->eip, err, vma->start, vma->end, vma->prot);
            kprintf("Process %d terminated (protection violation at 0x%08x).\n", proc->pid, faulting_addr);
        }
    }

    if (from_usermode) {
        scheduler_exit_current(139); /* never returns */
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

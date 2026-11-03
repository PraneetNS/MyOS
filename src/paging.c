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
#include "signal.h"
#include "tmpfs.h"
#include "kstack.h"
#include "kmutex.h"

extern void paging_flush(uint64_t pml4_phys);
extern void invlpg_page(uint64_t vaddr);

int smep_enabled = 0;
int smap_enabled = 0;

static volatile int test_smap_active = 0;
static volatile int test_smap_fault_occurred = 0;
static volatile uint64_t test_smap_resume_rip = 0;

static volatile int test_smep_active = 0;
static volatile int test_smep_fault_occurred = 0;
static volatile uint64_t test_smep_resume_rip = 0;

static void page_fault_handler(struct registers* regs) {
    uint64_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    int from_usermode = (regs->cs & 0x3) == 3;
    uint64_t err = regs->err_code;
    process_t* proc = scheduler_current();

    if (proc && is_kstack_guard_fault(faulting_addr, proc)) {
        if (kmutex_any_held_by_current()) {
            serial_printf("*** KERNEL PANIC: kernel stack overflow in pid %d (syscall %lu) with locks held! ***\n",
                          proc->pid, (unsigned long)proc->current_syscall);
            kprintf("\n*** KERNEL PANIC: kernel stack overflow in pid %d (syscall %lu) with locks held! ***\nSystem halted.\n",
                    proc->pid, (unsigned long)proc->current_syscall);
            asm volatile ("cli");
            for (;;) asm volatile ("hlt");
        }
        serial_printf("kernel stack overflow in pid %d (syscall %lu)\n",
                      proc->pid, (unsigned long)proc->current_syscall);
        kprintf("kernel stack overflow in pid %d (syscall %lu)\n",
                proc->pid, (unsigned long)proc->current_syscall);
        scheduler_exit_current(139);
        return;
    }

    int in_uaccess = ((uint64_t)regs->rip >= (uint64_t)copy_user_start && (uint64_t)regs->rip < (uint64_t)copy_user_end) ||
                     ((uint64_t)regs->rip >= (uint64_t)copy_user_start2 && (uint64_t)regs->rip < (uint64_t)copy_user_end2);

    /* 1. COW write fault */
    if ((err & 1) && (err & 2) && proc && !proc->is_kernel_task && faulting_addr < 0x0000800000000000ULL) {
        uint64_t pml4_idx = (faulting_addr >> 39) & 0x1FFULL;
        uint64_t pdpt_idx = (faulting_addr >> 30) & 0x1FFULL;
        uint64_t pd_idx   = (faulting_addr >> 21) & 0x1FFULL;
        uint64_t pt_idx   = (faulting_addr >> 12) & 0x1FFULL;

        if (pml4_idx < 256 && (proc->as.pml4[pml4_idx] & PAGE_PRESENT)) {
            uint64_t* pdpt = (uint64_t*) P2V(proc->as.pml4[pml4_idx] & ~0xFFFULL);
            if (pdpt[pdpt_idx] & PAGE_PRESENT) {
                uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);
                if (pd[pd_idx] & PAGE_PRESENT) {
                    uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);
                    uint64_t pte = pt[pt_idx];

                    if ((pte & PAGE_PRESENT) && (pte & PTE_COW)) {
                        uint64_t old_frame = pte & ~0xFFFULL & ~(1ULL << 63);
                        uint16_t rc = pmm_refcount(old_frame);
                        uint64_t flags = (pte & 0xFFFULL & ~PTE_COW) | PAGE_WRITE;
                        uint64_t nx = pte & (1ULL << 63);

                        if (rc <= 1) {
                            pt[pt_idx] = old_frame | flags | nx;
                        } else {
                            uint64_t new_frame = pmm_alloc_frame();
                            if (!new_frame) {
                                serial_printf("[pf] OOM during COW write fault for PID %d\n", proc->pid);
                                scheduler_exit_current(139);
                            }
                            const uint8_t* src_ptr = (const uint8_t*) P2V(old_frame);
                            uint8_t* dst_ptr = (uint8_t*) P2V(new_frame);
                            for (int i = 0; i < 4096; i++) dst_ptr[i] = src_ptr[i];

                            pmm_unref(old_frame);
                            pt[pt_idx] = new_frame | flags | nx;
                        }

                        invlpg_page(faulting_addr & ~0xFFFULL);
                        return; /* Handled COW */
                    }
                }
            }
        }
    }

    /* 2. Demand Paging & Stack Growth */
    if (!(err & 1) && proc && !proc->is_kernel_task) {
        if (faulting_addr >= USER_STACK_BOTTOM && faulting_addr < USER_STACK_TOP) {
            vma_t* v = proc->vma_list;
            while (v) {
                if (v->flags & VMA_FLAG_STACK) {
                    if (faulting_addr < v->start) {
                        v->start = faulting_addr & ~0xFFFULL;
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
                /* Write to non-writable VMA */
            } else if (!is_write && !(vma->prot & (VMA_PROT_READ | VMA_PROT_EXEC))) {
                /* Read from unreadable VMA */
            } else {
                uint64_t page_va = faulting_addr & ~0xFFFULL;
                uint64_t off_in_vma = page_va - vma->start;
                uint64_t file_offset = vma->offset + off_in_vma;

                /* Handle tmpfs files */
                if ((vma->flags & VMA_FLAG_FILE) && vma->file && tmpfs_is_tmpfs_vnode(vma->file)) {
                    tmpfs_node_t* tnode = (tmpfs_node_t*) vma->file->fs_data;
                    uint32_t cur_size = tnode ? tnode->size : vma->file->size;
                    if ((off_in_vma < vma->file_size || (vma->flags & VMA_FLAG_SHARED)) && file_offset >= cur_size) {
                        if (from_usermode) {
                            sig_send(proc, SIGBUS);
                            signal_handle_pending(regs);
                            return;
                        }
                        if (in_uaccess) {
                            regs->rip = (uint64_t) copy_user_fault;
                            return;
                        }
                    }

                    if (vma->flags & VMA_FLAG_SHARED) {
                        uint32_t page_idx = (uint32_t)(file_offset / FRAME_SIZE);
                        int alloc_err = 0;
                        uint64_t frame_phys = tmpfs_get_or_alloc_frame(tnode, page_idx, 1, &alloc_err);
                        if (!frame_phys || alloc_err != 0) {
                            if (from_usermode) {
                                sig_send(proc, SIGBUS);
                                signal_handle_pending(regs);
                                return;
                            }
                            if (in_uaccess) {
                                regs->rip = (uint64_t) copy_user_fault;
                                return;
                            }
                        }

                        pmm_ref(frame_phys);
                        uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
                        if (vma->prot & VMA_PROT_WRITE) pte_flags |= PAGE_WRITE;
                        if (!(vma->prot & VMA_PROT_EXEC)) pte_flags |= PAGE_NX;

                        if (vmm_map_page(&proc->as, page_va, frame_phys, pte_flags) != 0) {
                            pmm_unref(frame_phys);
                            scheduler_exit_current(139);
                            return;
                        }

                        invlpg_page(page_va);
                        return;
                    }
                }

                uint64_t frame_phys = pmm_alloc_frame();
                if (!frame_phys) {
                    serial_printf("[pf] OOM: cannot allocate frame for PID %d\n", proc->pid);
                    scheduler_exit_current(139);
                }

                uint8_t* page_ptr = (uint8_t*) P2V(frame_phys);
                for (int i = 0; i < 4096; i++) page_ptr[i] = 0;

                if ((vma->flags & VMA_FLAG_FILE) && vma->file) {
                    if (page_va < vma->start + vma->file_size) {
                        uint32_t to_read = 4096;
                        if (off_in_vma + to_read > vma->file_size) {
                            to_read = (uint32_t)(vma->file_size - off_in_vma);
                        }
                        if (vma->file->ops && vma->file->ops->read) {
                            vma->file->ops->read(vma->file, (uint32_t)file_offset, page_ptr, to_read);
                        }
                    }
                }

                uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
                if (vma->prot & VMA_PROT_WRITE) pte_flags |= PAGE_WRITE;
                if (!(vma->prot & VMA_PROT_EXEC)) pte_flags |= PAGE_NX;

                if (vmm_map_page(&proc->as, page_va, frame_phys, pte_flags) != 0) {
                    pmm_unref(frame_phys);
                    scheduler_exit_current(139);
                }

                invlpg_page(page_va);
                return;
            }
        }
    }

    if (test_smap_active && !(regs->err_code & 0x4)) {
        test_smap_fault_occurred = 1;
        test_smap_active = 0;
        serial_printf("[cpu-harden-test] SMAP fault intercepted: kernel touched user addr 0x%lx with AC=0 (err=0x%lx, rip=0x%lx)\n",
                      faulting_addr, regs->err_code, regs->rip);
        kprintf("[cpu-harden-test] SMAP fault intercepted: kernel touched user addr 0x%lx with AC=0 (err=0x%lx, rip=0x%lx)\n",
                faulting_addr, regs->err_code, regs->rip);
        regs->rip = test_smap_resume_rip;
        return;
    }

    if (test_smep_active && !(regs->err_code & 0x4)) {
        test_smep_fault_occurred = 1;
        test_smep_active = 0;
        serial_printf("[cpu-harden-test] SMEP fault intercepted: kernel executed user code at 0x%lx (err=0x%lx, rip=0x%lx)\n",
                      faulting_addr, regs->err_code, regs->rip);
        kprintf("[cpu-harden-test] SMEP fault intercepted: kernel executed user code at 0x%lx (err=0x%lx, rip=0x%lx)\n",
                faulting_addr, regs->err_code, regs->rip);
        regs->rip = test_smep_resume_rip;
        return;
    }

    if (in_uaccess) {
        regs->rip = (uint64_t) copy_user_fault;
        return;
    }

    /* 3. Unhandled Fault -> terminate process or panic */
    kprintf("\n*** PAGE FAULT at 0x%lx (err=0x%lx, %s, %s, rip=0x%lx) ***\n",
            (unsigned long)faulting_addr, (unsigned long)regs->err_code,
            (regs->err_code & 0x4) ? "user-mode" : "kernel-mode",
            (regs->err_code & 0x2) ? "write" : "read",
            (unsigned long)regs->rip);

    serial_printf("[pf-regs] rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx r8=%016lx r9=%016lx r10=%016lx r11=%016lx rip=%016lx cs=%016lx cr2=%016lx\n",
                  regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi, regs->rbp, regs->rsp,
                  regs->r8, regs->r9, regs->r10, regs->r11, regs->rip, regs->cs, faulting_addr);

    if (proc && !proc->is_kernel_task) {
        vma_t* vma = vma_find(proc, faulting_addr);
        if (!vma) {
            serial_printf("[pf] PID %d (%s) page fault at 0x%lx (rip=0x%lx, err=0x%lx): outside any VMA\n",
                          proc->pid, proc->name, (unsigned long)faulting_addr, (unsigned long)regs->rip, (unsigned long)err);
            serial_printf("Process %d terminated (page fault at 0x%08x, outside any VMA).\n", proc->pid, (uint32_t)faulting_addr);
            kprintf("Process %d terminated (page fault at 0x%08x, outside any VMA).\n", proc->pid, (uint32_t)faulting_addr);
        } else {
            serial_printf("[pf] PID %d (%s) page fault at 0x%lx: protection violation in VMA [0x%lx-0x%lx prot=0x%x]\n",
                          proc->pid, proc->name, (unsigned long)faulting_addr,
                          (unsigned long)vma->start, (unsigned long)vma->end, vma->prot);
        }
    }

    if (from_usermode) {
        if (proc) sig_send(proc, SIGSEGV);
        signal_handle_pending(regs);
        return;
    }

    kprintf("\n*** KERNEL PAGE FAULT at 0x%lx (err=0x%lx, rip=0x%lx) ***\n",
            (unsigned long)faulting_addr, (unsigned long)regs->err_code, (unsigned long)regs->rip);
    serial_printf("*** KERNEL PAGE FAULT at 0x%lx (err=0x%lx, rip=0x%lx, cs=0x%lx, rsp=0x%lx, rbp=0x%lx) ***\n",
                  (unsigned long)faulting_addr, (unsigned long)regs->err_code, (unsigned long)regs->rip,
                  (unsigned long)regs->cs, (unsigned long)regs->rsp, (unsigned long)regs->rbp);
    if (regs->rsp >= 0xffff800000000000ULL) {
        uint64_t* stk = (uint64_t*) regs->rsp;
        serial_printf("Stack dump at %p:\n", stk);
        for (int i = 0; i < 20; i++) {
            serial_printf("  [+%02x] %016lx\n", i * 8, stk[i]);
        }
    }
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

int test_smap_behavior(void* user_ptr) {
    if (!smap_enabled) {
        serial_printf("[cpu-harden-test] SMAP not supported by CPU, skipping behavioral fault test\n");
        return 2; /* 2 = SKIPPED */
    }
    test_smap_fault_occurred = 0;
    test_smap_active = 1;

    /* Ensure AC=0 */
    asm volatile ("clac" ::: "cc");

    /* Attempt to read user memory from supervisor mode without STAC */
    asm volatile (
        "lea 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movb (%1), %%al\n\t"
        "1:\n\t"
        : "=m"(test_smap_resume_rip)
        : "r"(user_ptr)
        : "rax", "memory"
    );

    test_smap_active = 0;
    if (test_smap_fault_occurred) {
        serial_printf("[cpu-harden-test] PASSED: SMAP hardware fault caught and verified\n");
        return 0;
    } else {
        serial_printf("[cpu-harden-test] FAILED: SMAP did not fault on user memory read\n");
        return -1;
    }
}

int test_smep_behavior(void* user_func) {
    if (!smep_enabled) {
        serial_printf("[cpu-harden-test] SMEP not supported by CPU, skipping behavioral fault test\n");
        return 2; /* 2 = SKIPPED */
    }
    test_smep_fault_occurred = 0;
    test_smep_active = 1;

    /* Attempt to execute user page from supervisor mode */
    asm volatile (
        "lea 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        : "=m"(test_smep_resume_rip)
        : "r"(user_func)
        : "rax", "memory"
    );

    test_smep_active = 0;
    if (test_smep_fault_occurred) {
        serial_printf("[cpu-harden-test] PASSED: SMEP hardware fault caught and verified\n");
        return 0;
    } else {
        serial_printf("[cpu-harden-test] FAILED: SMEP did not fault on jump to user code\n");
        return -1;
    }
}

/* Check CPUID and enable SMEP (Supervisor Mode Execution Prevention) and SMAP (Supervisor Mode Access Prevention) */
static void cpu_enable_smep_smap(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));

    uint64_t cr4;
    asm volatile ("mov %%cr4, %0" : "=r"(cr4));

    if (ebx & (1U << 7)) {
        cr4 |= (1ULL << 20); /* CR4.SMEP (bit 20) */
        smep_enabled = 1;
        kprintf("[cpu] SMEP enabled (CR4.SMEP=1)\n");
    }

    if (ebx & (1U << 20)) {
        cr4 |= (1ULL << 21); /* CR4.SMAP (bit 21) */
        smap_enabled = 1;
        kprintf("[cpu] SMAP enabled (CR4.SMAP=1)\n");
    }

    asm volatile ("mov %0, %%cr4" : : "r"(cr4));
}

void paging_init(void) {
    /* Reload CR3 with kernel PML4 */
    paging_flush(paging_get_kernel_pml4_phys());

    cpu_enable_smep_smap();

    register_interrupt_handler(14, &page_fault_handler);

    terminal_writestring("[ok] Paging initialized (x86-64 long mode 4-level paging active, NX/SMEP/SMAP ready)\n");
}

uint64_t paging_get_kernel_pml4_phys(void) {
    return (uint64_t)&boot_pml4;
}

uint64_t* paging_get_kernel_pml4_virt(void) {
    return (uint64_t*) P2V(&boot_pml4);
}

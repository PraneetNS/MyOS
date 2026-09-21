#include "paging.h"
#include "idt.h"
#include "vga.h"
#include "vmm.h"
#include "shell.h"

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4

/* Identity-map the first 16MB: 4 page tables x 1024 entries x 4KB = 16MB.
   NUM_TABLES=4 covers 0-16MB total. As of Stage 6, only directory entry 0
   (0-4MB, where the kernel image + heap live -- confirmed well under 4MB)
   is still identity-mapped this way, and it's now SUPERVISOR-ONLY: user
   processes can no longer touch kernel memory directly. Entries 1-3
   (4-16MB) are left PRESENT here purely as a physical-frame identity map
   the KERNEL itself uses to read/write freshly allocated frames while
   setting up a new process's address space (see vmm.c) -- they are NOT
   user-accessible, and a process's own page directory does not inherit
   them at all; vmm.c builds fresh mappings into that range per-process. */
#define NUM_TABLES 4

static uint32_t page_directory[1024]  __attribute__((aligned(4096)));
static uint32_t page_tables[NUM_TABLES][1024] __attribute__((aligned(4096)));

extern void paging_flush(uint32_t page_directory_phys);
extern void paging_enable(void);

static void print_hex(uint32_t v) {
    char hex[9]; hex[8] = '\0';
    const char* digits = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) { hex[i] = digits[v & 0xF]; v >>= 4; }
    terminal_writestring(hex);
}

static void page_fault_handler(struct registers* regs) {
    uint32_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    int from_usermode = (regs->cs & 0x3) == 3; /* RPL bits of the faulting CS */

    terminal_writestring("\n*** PAGE FAULT at 0x");
    print_hex(faulting_addr);
    terminal_writestring(" (err=0x");
    print_hex(regs->err_code);
    terminal_writestring(", ");
    terminal_writestring((regs->err_code & 0x4) ? "user-mode" : "kernel-mode");
    terminal_writestring(", ");
    terminal_writestring((regs->err_code & 0x2) ? "write" : "read");
    terminal_writestring(") ***\n");

    if (from_usermode) {
        /* A user process touched memory it has no mapping for -- exactly
           the protection this stage exists to add. Contain it: don't
           crash the whole OS, just refuse to let the process continue,
           restore the kernel's own address space, and hand control back
           to the shell. This is the real payoff of per-process page
           directories: kernel-space entry 0 is supervisor-only in every
           process's page directory, so this fault is expected and
           recoverable, not a bug. */
        terminal_writestring("Process terminated (illegal memory access). Returning to shell.\n\n");
        vmm_switch_to_kernel();
        asm volatile ("sti"); /* see syscall.c's SYS_EXIT for why this is required here too */
        shell_run(); /* never returns */
    }

    terminal_writestring("Kernel-mode page fault -- this is a real kernel bug. System halted.\n");
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

void paging_init(void) {
    for (int t = 0; t < NUM_TABLES; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys = (t * 1024 + i) * 4096;
            uint32_t flags = PAGE_PRESENT | PAGE_WRITE;
            if (t == 0) flags |= PAGE_USER; /* see NOTE below -- entry 0 is re-marked supervisor-only right after this loop */
            page_tables[t][i] = phys | flags;
        }
        page_directory[t] = ((uint32_t) &page_tables[t]) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    /* Entry 0 (0-4MB, kernel space) is supervisor-only: strip PAGE_USER
       from every page table entry in it. (Built as user-accessible above
       only to keep the loop symmetric; corrected here in one pass.) */
    for (int i = 0; i < 1024; i++)
        page_tables[0][i] &= ~((uint32_t)PAGE_USER);
    page_directory[0] &= ~((uint32_t)PAGE_USER);

    for (int i = NUM_TABLES; i < 1024; i++)
        page_directory[i] = 0;

    register_interrupt_handler(14, &page_fault_handler); /* vector 14 = #PF */

    paging_flush((uint32_t) page_directory); /* load CR3 */
    paging_enable();                          /* set CR0.PG */

    terminal_writestring("[ok] Paging enabled (kernel space 0-4MB supervisor-only)\n");
}

uint32_t paging_get_kernel_dir_entry0(void) { return page_directory[0]; }
uint32_t paging_get_kernel_dir_phys(void)   { return (uint32_t) page_directory; }

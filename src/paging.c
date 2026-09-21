#include "paging.h"
#include "idt.h"
#include "vga.h"

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2

/* Identity-map the first 16MB: 4 page tables x 1024 entries x 4KB = 16MB.
   That's comfortably more than our kernel + heap need for now, and this
   is the "early" identity map -- a proper kernel would later remap
   itself into a higher-half virtual address, but that's an optimization
   for a later stage, not a requirement to get paging working. */
#define NUM_TABLES 4

static uint32_t page_directory[1024]  __attribute__((aligned(4096)));
static uint32_t page_tables[NUM_TABLES][1024] __attribute__((aligned(4096)));

extern void paging_flush(uint32_t page_directory_phys);
extern void paging_enable(void);

static void page_fault_handler(struct registers* regs) {
    uint32_t faulting_addr;
    asm volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    terminal_writestring("\n*** PAGE FAULT at 0x");
    /* tiny inline hex printer -- no printf yet */
    char hex[9]; hex[8] = '\0';
    const char* digits = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        hex[i] = digits[faulting_addr & 0xF];
        faulting_addr >>= 4;
    }
    terminal_writestring(hex);
    terminal_writestring(" (error code shows present/write/user bits) ***\nSystem halted.\n");

    (void) regs;
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

void paging_init(void) {
    for (int t = 0; t < NUM_TABLES; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys = (t * 1024 + i) * 4096;
            page_tables[t][i] = phys | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[t] = ((uint32_t) &page_tables[t]) | PAGE_PRESENT | PAGE_WRITE;
    }

    /* Remaining directory entries: not present (yet) */
    for (int i = NUM_TABLES; i < 1024; i++)
        page_directory[i] = 0;

    register_interrupt_handler(14, &page_fault_handler); /* vector 14 = #PF */

    paging_flush((uint32_t) page_directory); /* load CR3 */
    paging_enable();                          /* set CR0.PG */

    terminal_writestring("[ok] Paging enabled (identity-mapped first 16MB)\n");
}

#include "tss.h"
#include "gdt.h"

/* We only need the fields the CPU actually reads on a privilege-level
   change (ss0/esp0) plus a couple of others; the rest can be zero since
   we're not using hardware task-switching, just the stack-switch feature. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct tss_entry tss;

void tss_install(int gdt_slot, uint16_t ss0, uint32_t esp0) {
    uint32_t base  = (uint32_t) &tss;
    uint32_t limit = base + sizeof(tss);

    /* 0xE9 = present, ring 3 accessible (needed so a user-mode `int`
       can find it), 32-bit TSS available type */
    gdt_set_gate(gdt_slot, base, limit, 0xE9, 0x00);

    for (uint8_t* p = (uint8_t*)&tss; p < (uint8_t*)&tss + sizeof(tss); p++)
        *p = 0;

    tss.ss0  = ss0;
    tss.esp0 = esp0;
    tss.cs   = 0x0B; /* kernel code selector | ring 3 RPL bits, per convention */
    tss.ss = tss.ds = tss.es = tss.fs = tss.gs = 0x13;
    tss.iomap_base = sizeof(tss);

    /* Load the task register: selector 0x28 (slot 5), ring 3 RPL bits set to 3 */
    uint16_t selector = (gdt_slot * 8) | 3;
    asm volatile ("ltr %0" : : "r"(selector));
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

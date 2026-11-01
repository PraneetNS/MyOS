#include "tss.h"
#include "gdt.h"
#include <stddef.h>

struct tss_entry64 tss;

/* IST stacks (8KB each for hardware safety) */
static uint8_t df_stack[8192];
static uint8_t nmi_stack[8192];
static uint8_t mc_stack[8192];

void tss_install(int gdt_slot, uint64_t rsp0) {
    uint8_t* p = (uint8_t*) &tss;
    for (size_t i = 0; i < sizeof(tss); i++) p[i] = 0;

    tss.rsp0 = rsp0;
    tss.ist1 = (uint64_t) &df_stack[sizeof(df_stack)];
    tss.ist2 = (uint64_t) &nmi_stack[sizeof(nmi_stack)];
    tss.ist3 = (uint64_t) &mc_stack[sizeof(mc_stack)];
    tss.iomap_base = sizeof(tss);

    uint64_t base  = (uint64_t) &tss;
    uint32_t limit = sizeof(tss) - 1;

    gdt_set_tss_gate(gdt_slot, base, limit);

    /* Load Task Register: selector 0x30 */
    uint16_t selector = (gdt_slot * 8);
    asm volatile ("ltr %0" : : "r"(selector));
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void tss_set_ist(int ist_index, uint64_t stack_top) {
    if (ist_index == 1) tss.ist1 = stack_top;
    else if (ist_index == 2) tss.ist2 = stack_top;
    else if (ist_index == 3) tss.ist3 = stack_top;
}

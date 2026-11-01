#ifndef TSS_H
#define TSS_H

#include <stdint.h>

struct tss_entry64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;   /* Double Fault Stack */
    uint64_t ist2;   /* NMI Stack */
    uint64_t ist3;   /* Machine Check Stack */
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern struct tss_entry64 tss;

void tss_install(int gdt_slot, uint64_t rsp0);
void tss_set_kernel_stack(uint64_t rsp0);
void tss_set_ist(int ist_index, uint64_t stack_top);

#endif

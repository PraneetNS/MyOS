#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void gdt_install(void);
void gdt_set_tss_gate(int num, uint64_t base, uint32_t limit);

#endif

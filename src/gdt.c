#include "gdt.h"

/* In 64-bit long mode, standard GDT descriptors are 8 bytes.
   TSS descriptors are expanded to 16 bytes.
   Selectors:
     0x00: Null descriptor
     0x08: Kernel Code (64-bit, Ring 0)
     0x10: Kernel Data (Ring 0)
     0x18: User Code 32-bit (for compatibility)
     0x20: User Data (Ring 3)
     0x28: User Code 64-bit (Ring 3)
     0x30: TSS Descriptor Low  (8 bytes)
     0x38: TSS Descriptor High (8 bytes)
*/

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GDT_ENTRIES 8
static uint64_t gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_pointer;

extern void gdt_flush(uint64_t gdt_ptr_addr);

void gdt_set_tss_gate(int num, uint64_t base, uint32_t limit) {
    /* 16-byte TSS descriptor in 64-bit mode occupying two 8-byte slots (num and num + 1) */
    uint64_t low = (limit & 0xFFFFULL) |
                   ((base & 0xFFFFFFULL) << 16) |
                   (0x89ULL << 40) |             /* Present, Available 64-bit TSS (Type 9) */
                   (((uint64_t)(limit >> 16) & 0xFULL) << 48) |
                   (((base >> 24) & 0xFFULL) << 56);
    uint64_t high = (base >> 32);

    gdt[num]     = low;
    gdt[num + 1] = high;

    gdt_flush((uint64_t) &gdt_pointer);
}

void gdt_install(void) {
    gdt_pointer.limit = (sizeof(uint64_t) * GDT_ENTRIES) - 1;
    gdt_pointer.base  = (uint64_t) &gdt;

    gdt[0] = 0x0000000000000000ULL;  /* 0x00: Null */
    gdt[1] = 0x00209A0000000000ULL;  /* 0x08: 64-bit Kernel Code (L=1, P=1, DPL=0) */
    gdt[2] = 0x0000920000000000ULL;  /* 0x10: 64-bit Kernel Data (W=1, P=1, DPL=0) */
    gdt[3] = 0x0000FA0000000000ULL;  /* 0x18: 32-bit User Code */
    gdt[4] = 0x0000F20000000000ULL;  /* 0x20: User Data (DPL=3, P=1) */
    gdt[5] = 0x0020FA0000000000ULL;  /* 0x28: 64-bit User Code (L=1, DPL=3, P=1) */
    gdt[6] = 0;                      /* 0x30: TSS Low */
    gdt[7] = 0;                      /* 0x38: TSS High */

    gdt_flush((uint64_t) &gdt_pointer);
}

#ifndef KSTACK_H
#define KSTACK_H

#include <stdint.h>
#include <stddef.h>

#define KSTACK_AREA_BASE   0xFFFFFFFFB0000000ULL
#define KSTACK_SLOT_SIZE   0x20000ULL    /* 128KB per process slot */
#define KSTACK_GUARD_SIZE  4096ULL       /* 4KB unmapped guard page */
#define KSTACK_PAINT_BYTE  0x5A

struct process;

void   kstack_init(void);
int    kstack_alloc(int slot, uint8_t** out_base, uint64_t* out_top);
void   kstack_free(int slot);
size_t kstack_get_hwm(struct process* p);
int    is_kstack_guard_fault(uint64_t fault_addr, struct process* p);
void   trigger_stack_overflow(volatile int d);

#endif

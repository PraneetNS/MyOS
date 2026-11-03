#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Snapshot of CPU state pushed by 64-bit ISR stubs before calling C */
struct registers {
    uint64_t ds;                                     /* offset 0 */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  /* offset 8..71 */
    uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;      /* offset 72..127 */
    uint64_t int_no, err_code;                       /* offset 128..143 (note: in push order) */
    uint64_t rip, cs, rflags, rsp, ss;               /* offset 136..175 (hardware iretq frame) */
} __attribute__((packed));

typedef void (*isr_handler_t)(struct registers*);

void idt_install(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);
int in_interrupt(void);

#endif

#include "idt.h"
#include "io.h"
#include "vga.h"
#include "serial.h"
#include "scheduler.h"
#include "signal.h"
#include "kstack.h"
#include "kmutex.h"

/* 16-byte IDT Gate Descriptor on x86-64 */
struct idt_entry64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;          /* Bits 0..2: IST stack index, Bits 3..7: 0 */
    uint8_t  type_attr;    /* P, DPL, Gate Type */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry64 idt[IDT_ENTRIES];
static struct idt_ptr64   idt_pointer;

static isr_handler_t interrupt_handlers[IDT_ENTRIES];

extern void idt_flush(uint64_t idt_ptr_addr);

/* Declared in isr.s */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void isr128(void);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = (uint16_t)(base & 0xFFFF);
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    if (num == 8)  idt[num].ist = 1; /* Double fault uses IST1 */
    if (num == 2)  idt[num].ist = 2; /* NMI uses IST2 */
    if (num == 18) idt[num].ist = 3; /* Machine check uses IST3 */

    idt[num].type_attr   = flags;
    idt[num].offset_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero        = 0;
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x0);  outb(0xA1, 0x0);
}

void idt_install(void) {
    idt_pointer.limit = sizeof(idt) - 1;
    idt_pointer.base  = (uint64_t) &idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        interrupt_handlers[i] = 0;

    pic_remap();

    /* 0x08 = 64-bit kernel code selector, 0x8E = 64-bit interrupt gate (DPL 0, P=1) */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E); idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E); idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E); idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E); idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E); idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E); idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E); idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E); idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E); idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E); idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E); idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E); idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E); idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E); idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E); idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E); idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E); idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E); idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E); idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E); idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E); idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E); idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E); idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E); idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

    /* System call gate: 0xEE = DPL 3 (user accessible) */
    idt_set_gate(128, (uint64_t)isr128, 0x08, 0xEE);

    idt_flush((uint64_t) &idt_pointer);
}

static const char* exception_messages[32] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
};

void isr_handler(struct registers* regs) {
    if (regs->int_no == 1) {
        process_t* cur_p = scheduler_current();
        serial_printf("\n*** HARDWARE WATCHPOINT HIT at rip=%016lx rsp=%016lx rbp=%016lx rax=%016lx rbx=%016lx rdx=%016lx rsi=%016lx rdi=%016lx (pid=%d %s) ***\n",
                      regs->rip, regs->rsp, regs->rbp, regs->rax, regs->rbx, regs->rdx, regs->rsi, regs->rdi,
                      cur_p ? cur_p->pid : -1, cur_p ? cur_p->name : "none");
        if (regs->rsp >= 0xffff800000000000ULL) {
            uint64_t* stk = (uint64_t*) regs->rsp;
            serial_printf("Stack dump at %p:\n", stk);
            for (int i = 0; i < 16; i++) {
                serial_printf("  [+%02x] %016lx\n", i * 8, stk[i]);
            }
        }
        /* Disable watchpoint so we can continue */
        asm volatile ("mov %0, %%dr7" : : "r"(0ULL));
        asm volatile ("mov %0, %%dr6" : : "r"(0ULL));
        return;
    }

    if (interrupt_handlers[regs->int_no] != 0) {
        interrupt_handlers[regs->int_no](regs);
        if ((regs->cs & 0x3) == 3) {
            signal_handle_pending(regs);
        }
        return;
    }

    if (regs->int_no == 8) {
        process_t* cur_p = scheduler_current();
        uint64_t cr2 = 0;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (cur_p && (is_kstack_guard_fault(cr2, cur_p) || is_kstack_guard_fault(regs->rsp, cur_p) ||
            (regs->rsp >= (uint64_t)cur_p->kernel_stack_base - 8192 && regs->rsp <= (uint64_t)cur_p->kernel_stack_base + 512))) {
            if (kmutex_any_held_by_current()) {
                serial_printf("*** KERNEL PANIC: kernel stack overflow in pid %d (syscall %lu) with locks held! ***\n",
                              cur_p->pid, (unsigned long)cur_p->current_syscall);
                kprintf("\n*** KERNEL PANIC: kernel stack overflow in pid %d (syscall %lu) with locks held! ***\nSystem halted.\n",
                        cur_p->pid, (unsigned long)cur_p->current_syscall);
                asm volatile ("cli");
                for (;;) asm volatile ("hlt");
            }
            serial_printf("kernel stack overflow in pid %d (syscall %lu)\n",
                          cur_p->pid, (unsigned long)cur_p->current_syscall);
            kprintf("kernel stack overflow in pid %d (syscall %lu)\n",
                    cur_p->pid, (unsigned long)cur_p->current_syscall);
            scheduler_exit_current(139);
            return;
        }
    }

    int from_usermode = (regs->cs & 0x3) == 3;
    if (from_usermode) {
        process_t* proc = scheduler_current();
        serial_printf("[fault] PID %d (%s) exception %lu (%s) at rip=0x%lx rsp=0x%lx rbp=0x%lx err=0x%lx\n",
                      proc ? proc->pid : 0, proc ? proc->name : "user",
                      (unsigned long)regs->int_no,
                      (regs->int_no < 32) ? exception_messages[regs->int_no] : "unknown",
                      (unsigned long)regs->rip, (unsigned long)regs->rsp, (unsigned long)regs->rbp, (unsigned long)regs->err_code);

        int sig = SIGSEGV;
        if (regs->int_no == 0 || regs->int_no == 16 || regs->int_no == 19) sig = SIGFPE;
        else if (regs->int_no == 4 || regs->int_no == 6) sig = SIGILL;
        else if (regs->int_no == 13) sig = SIGSEGV;

        if (proc) {
            sig_send(proc, sig);
        }
        signal_handle_pending(regs);
        return;
    }

    process_t* cur_p = scheduler_current();
    serial_printf("*** KERNEL PANIC: %s (vec=%lu, err=0x%lx, rip=0x%lx, pid=%d (%s))\n",
                  (regs->int_no < 32) ? exception_messages[regs->int_no] : "Unknown",
                  (unsigned long)regs->int_no, (unsigned long)regs->err_code, (unsigned long)regs->rip,
                  cur_p ? cur_p->pid : -1, cur_p ? cur_p->name : "none");
    serial_printf("Registers: rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx r8=%016lx r9=%016lx r10=%016lx r11=%016lx r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n",
                  regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi, regs->rbp, regs->rsp,
                  regs->r8, regs->r9, regs->r10, regs->r11, regs->r12, regs->r13, regs->r14, regs->r15);
    if (regs->rsp >= 0xffff800000000000ULL) {
        uint64_t* stk = (uint64_t*) regs->rsp;
        serial_printf("Stack dump at %p:\n", stk);
        for (int i = 0; i < 16; i++) {
            serial_printf("  [+%02x] %016lx\n", i * 8, stk[i]);
        }
    }
    kprintf("\n*** KERNEL PANIC: %s (vec=%lu, err=0x%lx, rip=0x%lx) ***\nSystem halted.\n",
            (regs->int_no < 32) ? exception_messages[regs->int_no] : "Unknown",
            (unsigned long)regs->int_no, (unsigned long)regs->err_code, (unsigned long)regs->rip);
    serial_printf("*** KERNEL PANIC: %s (vec=%lu, err=0x%lx, rip=0x%lx) ***\nSystem halted.\n",
                  (regs->int_no < 32) ? exception_messages[regs->int_no] : "Unknown",
                  (unsigned long)regs->int_no, (unsigned long)regs->err_code, (unsigned long)regs->rip);
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

int in_interrupt(void) {
    process_t* cur = scheduler_current();
    if (!cur) return 0;
    return cur->in_interrupt > 0;
}

void irq_handler(struct registers* regs) {
    process_t* cur = scheduler_current();
    if (cur) cur->in_interrupt++;

    if (regs->int_no >= 40)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if (interrupt_handlers[regs->int_no] != 0)
        interrupt_handlers[regs->int_no](regs);

    cur = scheduler_current();
    if (cur && cur->in_interrupt > 0) cur->in_interrupt--;
    if ((regs->cs & 0x3) == 3 && cur) {
        cur->in_interrupt = 0;
    }

    if ((regs->cs & 0x3) == 3) {
        signal_handle_pending(regs);
    }
}

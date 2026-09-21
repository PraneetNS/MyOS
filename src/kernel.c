#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "usermode.h"
#include "usermode_demo.h"
#include "task.h"
#include "demo_tasks.h"

static void print_uint(uint32_t n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { terminal_writestring("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(&buf[i]);
}

#define KERNEL_STACK_SIZE 8192
#define USER_STACK_SIZE   8192

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    (void) magic;

    terminal_initialize();
    terminal_writestring("MyOS kernel booted successfully.\n");

    gdt_install();
    terminal_writestring("[ok] GDT installed\n");

    idt_install();
    terminal_writestring("[ok] IDT installed, PIC remapped\n");

    timer_install(100);
    terminal_writestring("[ok] PIT timer installed (100Hz)\n");

    keyboard_install();
    terminal_writestring("[ok] Keyboard driver installed\n");

    pmm_init(mb_info_addr);
    paging_init();
    kheap_init();

    asm volatile ("sti");
    terminal_writestring("[ok] Interrupts enabled\n");

    terminal_writestring("Free physical frames: ");
    print_uint(pmm_free_frame_count());
    terminal_writestring("\n");

    /* --- Stage 4: TSS, ring 3, syscalls, scheduler --- */

    tss_install(5, 0x10, 0); /* slot 5 in the GDT, kernel data selector for ss0 */
    terminal_writestring("[ok] TSS installed\n");

    syscall_install();
    terminal_writestring("[ok] Syscall interface installed (int 0x80)\n");

    /* Kernel-side stack the CPU will switch to on the demo's ring3->ring0
       transitions (the two sys_write/sys_exit syscalls it makes). */
    uint8_t* kstack = (uint8_t*) kmalloc(KERNEL_STACK_SIZE);
    tss_set_kernel_stack((uint32_t)(kstack + KERNEL_STACK_SIZE));

    uint8_t* ustack = (uint8_t*) kmalloc(USER_STACK_SIZE);
    uint32_t user_stack_top = (uint32_t)(ustack + USER_STACK_SIZE);

    /* Register the two kernel-mode demo tasks the scheduler will run
       once the ring-3 demo below calls sys_exit(). */
    task_create(task_a_entry);
    task_create(task_b_entry);

    terminal_writestring("\nEntering ring 3...\n");
    enter_usermode((uint32_t) usermode_demo_entry, user_stack_top);

    /* enter_usermode never returns -- sys_exit's handler calls
       scheduler_start(), which takes over permanently. This line is
       unreachable but kept as a documented safety net. */
    for (;;) { asm volatile ("hlt"); }
}

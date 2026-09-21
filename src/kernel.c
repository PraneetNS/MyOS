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
#include "ata.h"
#include "fs.h"
#include "shell.h"

static void print_uint(uint32_t n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { terminal_writestring("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(&buf[i]);
}

#define KERNEL_STACK_SIZE 8192

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

    tss_install(5, 0x10, 0);
    terminal_writestring("[ok] TSS installed\n");

    /* Kernel-side stack used for any ring3->ring0 transition (every
       syscall a shell-launched program makes). Set once here; there's
       only ever one "current" user-mode program at a time in this
       design, so a single shared kernel stack for the TSS is fine. */
    uint8_t* kstack = (uint8_t*) kmalloc(KERNEL_STACK_SIZE);
    tss_set_kernel_stack((uint32_t)(kstack + KERNEL_STACK_SIZE));

    syscall_install();
    terminal_writestring("[ok] Syscall interface installed (int 0x80)\n");

    fs_init();

    shell_run(); /* never returns */

    for (;;) { asm volatile ("hlt"); }
}

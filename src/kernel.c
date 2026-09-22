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
#include "process.h"
#include "scheduler.h"
#include "pipe.h"

static void print_uint(uint32_t n) {
    char buf[11]; int i = 10; buf[10] = '\0';
    if (n == 0) { terminal_writestring("0"); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(&buf[i]);
}

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
    /* TSS.esp0 is updated per-process by the scheduler on every switch
       (see scheduler.c's enter_process()) -- no one-shot setup needed
       here anymore, unlike Stage 5/6. */

    syscall_install();
    terminal_writestring("[ok] Syscall interface installed (int 0x80)\n");

    fs_init();

    pipe_init();
    terminal_writestring("[ok] Pipe (IPC) initialized\n");

    process_init_table();
    terminal_writestring("[ok] Process table initialized (shell = process 0)\n");

    scheduler_start(); /* never returns -- becomes the shell, and from here
                           on, processes launched via 'run' are real,
                           independently scheduled tasks */

    for (;;) { asm volatile ("hlt"); }
}

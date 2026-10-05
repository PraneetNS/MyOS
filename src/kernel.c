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

#include "serial.h"

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    (void) magic;

    serial_init();
    terminal_initialize();
    kprintf("MyOS kernel booted successfully.\n");

    gdt_install();
    kprintf("[ok] GDT installed\n");

    idt_install();
    kprintf("[ok] IDT installed, PIC remapped\n");

    timer_install(100);
    kprintf("[ok] PIT timer installed (100Hz)\n");

    keyboard_install();
    kprintf("[ok] Keyboard driver installed\n");

    pmm_init(mb_info_addr);
    paging_init();
    kheap_init();

    asm volatile ("sti");
    kprintf("[ok] Interrupts enabled\n");

    kprintf("Free physical frames: %u\n", pmm_free_frame_count());

    tss_install(5, 0x10, 0);
    kprintf("[ok] TSS installed\n");
    /* TSS.esp0 is updated per-process by the scheduler on every switch
       (see scheduler.c's enter_process()) -- no one-shot setup needed
       here anymore, unlike Stage 5/6. */

    syscall_install();
    kprintf("[ok] Syscall interface installed (int 0x80)\n");

    fs_init();

    pipe_init();
    kprintf("[ok] Pipe (IPC) initialized\n");

    process_init_table();
    kprintf("[ok] Process table initialized (shell = process 0)\n");

    kprintf("BOOT OK\n");

    scheduler_start(); /* never returns -- becomes the shell, and from here
                           on, processes launched via 'run' are real,
                           independently scheduled tasks */

    for (;;) { asm volatile ("hlt"); }
}

#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    (void) magic;
    (void) mb_info_addr;

    terminal_initialize();
    terminal_writestring("MyOS kernel booted successfully.\n");

    gdt_install();
    terminal_writestring("[ok] GDT installed\n");

    idt_install();
    terminal_writestring("[ok] IDT installed, PIC remapped\n");

    timer_install(100); /* 100 Hz tick */
    terminal_writestring("[ok] PIT timer installed (100Hz)\n");

    keyboard_install();
    terminal_writestring("[ok] Keyboard driver installed\n");

    asm volatile ("sti"); /* enable interrupts -- everything above is now live */
    terminal_writestring("[ok] Interrupts enabled\n");

    terminal_writestring("\nStage 2 complete. Type something:\n> ");

    for (;;) {
        asm volatile ("hlt"); /* sleep until the next interrupt wakes us */
    }
}

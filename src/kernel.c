#include "vga.h"

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    (void) magic;
    (void) mb_info_addr;

    terminal_initialize();
    terminal_writestring("MyOS kernel booted successfully.\n");
    terminal_writestring("Stage 1 complete: protected mode, GDT set up by GRUB, VGA driver alive.\n");
    terminal_writestring("Next up: our own GDT/IDT, interrupts, and memory paging.\n");

    /* Nothing left to do yet -- halt the CPU until an interrupt arrives */
    for (;;) {
        asm volatile ("hlt");
    }
}

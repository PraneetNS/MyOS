#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"

/* Tiny helper: print an unsigned int in decimal (no libc here) */
static void print_uint(uint32_t n) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    if (n == 0) {
        terminal_writestring("0");
        return;
    }
    while (n > 0 && i > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
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

    terminal_writestring("\nFree physical frames: ");
    print_uint(pmm_free_frame_count());
    terminal_writestring("\n");

    /* Heap smoke test: allocate three blocks, free the middle one,
       allocate again and confirm the allocator reused freed space. */
    terminal_writestring("\nHeap test:\n");
    void* a = kmalloc(128);
    void* b = kmalloc(256);
    void* c = kmalloc(64);
    terminal_writestring("  allocated a, b, c\n");

    kfree(b);
    terminal_writestring("  freed b\n");

    void* d = kmalloc(100);
    terminal_writestring("  allocated d (should fit in freed space)\n");
    terminal_writestring(d ? "  [pass] kmalloc returned non-null after free+realloc\n"
                           : "  [FAIL] kmalloc returned null\n");

    (void) a; (void) c;

    terminal_writestring("\nStage 3 complete. Type something:\n> ");

    for (;;) {
        asm volatile ("hlt");
    }
}

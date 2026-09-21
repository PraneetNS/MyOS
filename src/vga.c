#include "vga.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_LIGHT_GREEN, VGA_BLACK);
    terminal_buffer = (uint16_t*) 0xB8000;   /* VGA text buffer's physical address */

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

static void terminal_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];

    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);

    terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
        return;
    }

    terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] = vga_entry(c, terminal_color);

    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
    }
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

/* terminal_row/column/buffer are shared mutable state. With preemptive
   multitasking now in the picture (Stage 4), two contexts could
   interleave writes to them mid-string and corrupt the display -- so
   each full string write is made a small critical section. This is a
   coarse fix (a real kernel would use a proper spinlock for SMP); on
   a single CPU, disabling interrupts around the write is sufficient. */
void terminal_writestring(const char* data) {
    size_t len = 0;
    while (data[len]) len++;

    uint32_t saved_eflags;
    asm volatile ("pushf; pop %0; cli" : "=r"(saved_eflags));

    terminal_write(data, len);

    if (saved_eflags & 0x200) /* only re-enable if it was enabled before */
        asm volatile ("sti");
}

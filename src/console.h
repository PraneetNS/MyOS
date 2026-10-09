#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>

/* Standard VGA text-mode colors */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14,
    VGA_WHITE = 15,
};

/* Console display modes (KDSETMODE / KDGETMODE) */
#define KD_TEXT         0x00
#define KD_GRAPHICS     0x01
#define KDGETMODE       0x4B3B
#define KDSETMODE       0x4B3C

/* Keyboard modes (KDSKBMODE / KDGKBMODE) */
#define K_RAW           0x00
#define K_XLATE         0x01
#define K_MEDIUMRAW     0x02
#define K_UNICODE       0x03
#define K_OFF           0x04
#define KDGKBMODE       0x4B44
#define KDSKBMODE       0x4B45

/* Console backend operations */
struct console_ops {
    const char* name;
    size_t cols;
    size_t rows;
    void (*init)(void);
    void (*putc)(size_t col, size_t row, unsigned char c, uint8_t color);
    void (*scroll)(uint8_t color);
    void (*set_cursor)(size_t col, size_t row);
    void (*clear)(uint8_t color);
    void (*set_color)(uint8_t color);
};

/* Console management API */
void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);

void console_switch_to_fb(void);
int  console_set_mode(int mode);
int  console_get_mode(void);
void console_restore_text_mode(void);
void console_on_process_exit(int pid);
void console_repaint(void);

int  console_set_kbmode(int mode);
int  console_get_kbmode(void);

size_t console_get_cols(void);
size_t console_get_rows(void);

#endif

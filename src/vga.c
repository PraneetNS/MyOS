#include "vga.h"
#include "paging.h"
#include "io.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

static enum vga_color fg_color = VGA_LIGHT_GREEN;
static enum vga_color bg_color = VGA_BLACK;
static int sgr_bold = 0;
static int sgr_reverse = 0;

/* ANSI Parser State */
typedef enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI
} ansi_state_t;

static ansi_state_t parser_state = ANSI_STATE_NORMAL;
#define MAX_CSI_PARAMS 8
static int csi_params[MAX_CSI_PARAMS];
static int csi_param_count = 0;
static int csi_has_param = 0;

static const enum vga_color ansi_color_map[8] = {
    VGA_BLACK,      /* 0: Black */
    VGA_RED,        /* 1: Red */
    VGA_GREEN,      /* 2: Green */
    VGA_BROWN,      /* 3: Yellow/Brown */
    VGA_BLUE,       /* 4: Blue */
    VGA_MAGENTA,    /* 5: Magenta */
    VGA_CYAN,       /* 6: Cyan */
    VGA_LIGHT_GREY  /* 7: White/Light Grey */
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t) fg | (uint8_t) (bg << 4);
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_enable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

static void update_terminal_color(void) {
    enum vga_color fg = fg_color;
    enum vga_color bg = bg_color;

    if (sgr_bold && fg < 8) {
        fg += 8;
    }
    if (sgr_reverse) {
        enum vga_color tmp = fg;
        fg = bg;
        bg = tmp;
    }
    terminal_color = vga_entry_color(fg, bg);
}

static void terminal_scroll(void) {
    /* Fast scrollback-free path using 32-bit word copies */
    uint32_t* dst = (uint32_t*) terminal_buffer;
    const uint32_t* src = (const uint32_t*) (terminal_buffer + VGA_WIDTH);
    for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH / 2; i++) {
        dst[i] = src[i];
    }

    uint16_t blank = vga_entry(' ', terminal_color);
    uint32_t blank2 = ((uint32_t)blank) | (((uint32_t)blank) << 16);
    uint32_t* last_row = (uint32_t*) (terminal_buffer + (VGA_HEIGHT - 1) * VGA_WIDTH);
    for (size_t x = 0; x < VGA_WIDTH / 2; x++) {
        last_row[x] = blank2;
    }

    terminal_row = VGA_HEIGHT - 1;
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    fg_color = VGA_LIGHT_GREEN;
    bg_color = VGA_BLACK;
    sgr_bold = 0;
    sgr_reverse = 0;
    update_terminal_color();
    terminal_buffer = (uint16_t*) P2V(0xB8000);   /* VGA text buffer in direct map */

    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);

    vga_enable_cursor();
    vga_update_cursor();
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

static void handle_sgr(int param) {
    if (param == 0) {
        /* Reset */
        fg_color = VGA_LIGHT_GREEN;
        bg_color = VGA_BLACK;
        sgr_bold = 0;
        sgr_reverse = 0;
    } else if (param == 1) {
        sgr_bold = 1;
    } else if (param == 7) {
        sgr_reverse = 1;
    } else if (param == 22) {
        sgr_bold = 0;
    } else if (param == 27) {
        sgr_reverse = 0;
    } else if (param >= 30 && param <= 37) {
        fg_color = ansi_color_map[param - 30];
    } else if (param == 39) {
        fg_color = VGA_LIGHT_GREEN;
    } else if (param >= 40 && param <= 47) {
        bg_color = ansi_color_map[param - 40];
    } else if (param == 49) {
        bg_color = VGA_BLACK;
    }
    update_terminal_color();
}

void terminal_putchar(char c) {
    if (parser_state == ANSI_STATE_NORMAL) {
        if (c == 0x1B) { /* ESC */
            parser_state = ANSI_STATE_ESC;
            return;
        }

        if (c == '\r') {
            terminal_column = 0;
            vga_update_cursor();
            return;
        }

        if (c == '\n') {
            terminal_column = 0;
            if (++terminal_row == VGA_HEIGHT)
                terminal_scroll();
            vga_update_cursor();
            return;
        }

        if (c == '\b') {
            if (terminal_column > 0)
                terminal_column--;
            vga_update_cursor();
            return;
        }

        if (c == '\t') {
            terminal_column = (terminal_column + 8) & ~7;
            if (terminal_column >= VGA_WIDTH) {
                terminal_column = 0;
                if (++terminal_row == VGA_HEIGHT)
                    terminal_scroll();
            }
            vga_update_cursor();
            return;
        }

        if ((unsigned char)c >= 32) {
            terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] = vga_entry((unsigned char)c, terminal_color);
            if (++terminal_column == VGA_WIDTH) {
                terminal_column = 0;
                if (++terminal_row == VGA_HEIGHT)
                    terminal_scroll();
            }
            vga_update_cursor();
            return;
        }
        return;
    }

    if (parser_state == ANSI_STATE_ESC) {
        if (c == '[') {
            parser_state = ANSI_STATE_CSI;
            csi_param_count = 0;
            csi_params[0] = 0;
            csi_has_param = 0;
            return;
        }
        parser_state = ANSI_STATE_NORMAL;
        return;
    }

    if (parser_state == ANSI_STATE_CSI) {
        if (c >= '0' && c <= '9') {
            csi_params[csi_param_count] = csi_params[csi_param_count] * 10 + (c - '0');
            csi_has_param = 1;
            return;
        }

        if (c == ';') {
            if (csi_param_count < MAX_CSI_PARAMS - 1) {
                csi_param_count++;
                csi_params[csi_param_count] = 0;
            }
            return;
        }

        /* Final command character */
        if (csi_has_param) {
            csi_param_count++;
        }

        switch (c) {
            case 'A': { /* Cursor Up */
                int n = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] : 1;
                if (terminal_row >= (size_t)n) terminal_row -= n;
                else terminal_row = 0;
                break;
            }
            case 'B': { /* Cursor Down */
                int n = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] : 1;
                terminal_row += n;
                if (terminal_row >= VGA_HEIGHT) terminal_row = VGA_HEIGHT - 1;
                break;
            }
            case 'C': { /* Cursor Forward */
                int n = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] : 1;
                terminal_column += n;
                if (terminal_column >= VGA_WIDTH) terminal_column = VGA_WIDTH - 1;
                break;
            }
            case 'D': { /* Cursor Back */
                int n = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] : 1;
                if (terminal_column >= (size_t)n) terminal_column -= n;
                else terminal_column = 0;
                break;
            }
            case 'H':
            case 'f': { /* Cursor Position CSI row;col H */
                int r = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] - 1 : 0;
                int col = (csi_param_count > 1 && csi_params[1] > 0) ? csi_params[1] - 1 : 0;
                if (r < 0) r = 0;
                if (r >= (int)VGA_HEIGHT) r = VGA_HEIGHT - 1;
                if (col < 0) col = 0;
                if (col >= (int)VGA_WIDTH) col = VGA_WIDTH - 1;
                terminal_row = r;
                terminal_column = col;
                break;
            }
            case 'J': { /* Erase Display */
                int mode = (csi_param_count > 0) ? csi_params[0] : 0;
                if (mode == 2) {
                    for (size_t y = 0; y < VGA_HEIGHT; y++)
                        for (size_t x = 0; x < VGA_WIDTH; x++)
                            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                    terminal_row = 0;
                    terminal_column = 0;
                } else if (mode == 0) {
                    for (size_t x = terminal_column; x < VGA_WIDTH; x++)
                        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                    for (size_t y = terminal_row + 1; y < VGA_HEIGHT; y++)
                        for (size_t x = 0; x < VGA_WIDTH; x++)
                            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                } else if (mode == 1) {
                    for (size_t y = 0; y < terminal_row; y++)
                        for (size_t x = 0; x < VGA_WIDTH; x++)
                            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                    for (size_t x = 0; x <= terminal_column; x++)
                        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                }
                break;
            }
            case 'K': { /* Erase in Line */
                int mode = (csi_param_count > 0) ? csi_params[0] : 0;
                if (mode == 0) {
                    for (size_t x = terminal_column; x < VGA_WIDTH; x++)
                        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                } else if (mode == 1) {
                    for (size_t x = 0; x <= terminal_column; x++)
                        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                } else if (mode == 2) {
                    for (size_t x = 0; x < VGA_WIDTH; x++)
                        terminal_buffer[terminal_row * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                }
                break;
            }
            case 'm': { /* SGR Color */
                if (csi_param_count == 0) {
                    handle_sgr(0);
                } else {
                    for (int i = 0; i < csi_param_count; i++) {
                        handle_sgr(csi_params[i]);
                    }
                }
                break;
            }
            default:
                break;
        }

        parser_state = ANSI_STATE_NORMAL;
        vga_update_cursor();
    }
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    size_t len = 0;
    while (data[len]) len++;

    uint32_t saved_eflags;
    asm volatile ("pushf; pop %0; cli" : "=r"(saved_eflags));

    terminal_write(data, len);

    if (saved_eflags & 0x200)
        asm volatile ("sti");
}

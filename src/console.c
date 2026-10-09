#include "console.h"
#include "fb.h"
#include "font.h"
#include "paging.h"
#include "pmm.h"
#include "io.h"
#include "serial.h"
#include "scheduler.h"
#include "errno.h"

#include <string.h>

/* --- Palette & Color Mapping --- */
static const uint32_t vga_to_rgb32[16] = {
    0x00000000, /* 0: VGA_BLACK */
    0x000000AA, /* 1: VGA_BLUE */
    0x0000AA00, /* 2: VGA_GREEN */
    0x0000AAAA, /* 3: VGA_CYAN */
    0x00AA0000, /* 4: VGA_RED */
    0x00AA00AA, /* 5: VGA_MAGENTA */
    0x00AA5500, /* 6: VGA_BROWN */
    0x00AAAAAA, /* 7: VGA_LIGHT_GREY */
    0x00555555, /* 8: VGA_DARK_GREY */
    0x005555FF, /* 9: VGA_LIGHT_BLUE */
    0x0055FF55, /* 10: VGA_LIGHT_GREEN */
    0x0055FFFF, /* 11: VGA_LIGHT_CYAN */
    0x00FF5555, /* 12: VGA_LIGHT_RED */
    0x00FF55FF, /* 13: VGA_LIGHT_MAGENTA */
    0x00FFFF55, /* 14: VGA_LIGHT_BROWN / YELLOW */
    0x00FFFFFF  /* 15: VGA_WHITE */
};

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

/* --- Global Console & Terminal State --- */
static size_t terminal_row = 0;
static size_t terminal_column = 0;
static uint8_t terminal_color = 0x07; /* Light grey on black */

static enum vga_color fg_color = VGA_LIGHT_GREY;
static enum vga_color bg_color = VGA_BLACK;
static int sgr_bold = 0;
static int sgr_reverse = 0;

static int g_kd_mode = KD_TEXT;
static int g_kb_mode = K_XLATE;
static int g_graphics_pid = 0;

/* Forward declarations */
static struct console_ops vga_text_ops;
static struct console_ops fb_text_ops;
static struct console_ops* current_ops = &vga_text_ops;

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t) fg | (uint8_t) (bg << 4);
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | ((uint16_t) color << 8);
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
    if (current_ops && current_ops->set_color) {
        current_ops->set_color(terminal_color);
    }
}

/* ========================================================================= */
/*                          VGA TEXT BACKEND (80x25)                         */
/* ========================================================================= */

static uint16_t* vga_buffer = NULL;

static void vga_update_hw_cursor(size_t col, size_t row) {
    uint16_t pos = (uint16_t)(row * 80 + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_enable_hw_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
}

static void vga_backend_init(void) {
    vga_buffer = (uint16_t*) P2V(0xB8000);
    uint16_t blank = vga_entry(' ', terminal_color);
    for (size_t i = 0; i < 80 * 25; i++) {
        vga_buffer[i] = blank;
    }
    vga_enable_hw_cursor();
    vga_update_hw_cursor(0, 0);
}

static void vga_backend_putc(size_t col, size_t row, unsigned char c, uint8_t color) {
    if (col >= 80 || row >= 25 || !vga_buffer) return;
    vga_buffer[row * 80 + col] = vga_entry(c, color);
}

static void vga_backend_scroll(uint8_t color) {
    if (!vga_buffer) return;
    uint32_t* dst = (uint32_t*) vga_buffer;
    const uint32_t* src = (const uint32_t*) (vga_buffer + 80);
    for (size_t i = 0; i < 24 * 80 / 2; i++) {
        dst[i] = src[i];
    }
    uint16_t blank = vga_entry(' ', color);
    uint32_t blank2 = ((uint32_t)blank) | (((uint32_t)blank) << 16);
    uint32_t* last_row = (uint32_t*) (vga_buffer + 24 * 80);
    for (size_t x = 0; x < 80 / 2; x++) {
        last_row[x] = blank2;
    }
}

static void vga_backend_set_cursor(size_t col, size_t row) {
    vga_update_hw_cursor(col, row);
}

static void vga_backend_clear(uint8_t color) {
    if (!vga_buffer) return;
    uint16_t blank = vga_entry(' ', color);
    for (size_t i = 0; i < 80 * 25; i++) {
        vga_buffer[i] = blank;
    }
}

static void vga_backend_set_color(uint8_t color) {
    (void) color;
}

static struct console_ops vga_text_ops = {
    .name = "vga_text",
    .cols = 80,
    .rows = 25,
    .init = vga_backend_init,
    .putc = vga_backend_putc,
    .scroll = vga_backend_scroll,
    .set_cursor = vga_backend_set_cursor,
    .clear = vga_backend_clear,
    .set_color = vga_backend_set_color,
};

/* ========================================================================= */
/*                     FRAMEBUFFER TEXT BACKEND (128x48)                     */
/* ========================================================================= */

#define FB_COLS 128
#define FB_ROWS 48
#define FB_GLYPH_WIDTH 8
#define FB_GLYPH_HEIGHT 16

struct fb_cell {
    unsigned char c;
    uint8_t color;
};

static struct fb_cell fb_cells[FB_ROWS][FB_COLS];
static uint32_t* fb_shadow = NULL;
static size_t fb_cursor_col = 0;
static size_t fb_cursor_row = 0;
static int fb_cursor_visible = 1;

static void fb_flush_rows(size_t start_row, size_t end_row) {
    if (g_kd_mode != KD_TEXT || !fb_shadow || !g_fb_info.virt_addr) return;
    if (start_row >= FB_ROWS) return;
    if (end_row >= FB_ROWS) end_row = FB_ROWS - 1;

    size_t start_y = start_row * FB_GLYPH_HEIGHT;
    size_t end_y = (end_row + 1) * FB_GLYPH_HEIGHT;
    uint32_t* vram = (uint32_t*) g_fb_info.virt_addr;

    for (size_t y = start_y; y < end_y; y++) {
        memcpy(&vram[y * 1024], &fb_shadow[y * 1024], 1024 * sizeof(uint32_t));
    }
}

static void fb_draw_cell_glyph(size_t col, size_t row, unsigned char c, uint8_t color, int is_cursor) {
    if (!fb_shadow || col >= FB_COLS || row >= FB_ROWS) return;

    uint32_t fg = vga_to_rgb32[color & 0x0F];
    uint32_t bg = vga_to_rgb32[(color >> 4) & 0x0F];

    if (is_cursor) {
        /* Inverted visible block cursor */
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
        if (fg == bg) {
            bg = 0x00AAAAAA;
            fg = 0x00000000;
        }
    }

    const uint8_t* glyph = font_get_glyph(c);
    size_t px = col * FB_GLYPH_WIDTH;
    size_t py = row * FB_GLYPH_HEIGHT;

    for (size_t y = 0; y < 16; y++) {
        uint8_t bits = glyph[y];
        uint32_t* dst = &fb_shadow[(py + y) * 1024 + px];
        dst[0] = (bits & 0x80) ? fg : bg;
        dst[1] = (bits & 0x40) ? fg : bg;
        dst[2] = (bits & 0x20) ? fg : bg;
        dst[3] = (bits & 0x10) ? fg : bg;
        dst[4] = (bits & 0x08) ? fg : bg;
        dst[5] = (bits & 0x04) ? fg : bg;
        dst[6] = (bits & 0x02) ? fg : bg;
        dst[7] = (bits & 0x01) ? fg : bg;
    }
}

static void fb_backend_init(void) {
    if (!fb_shadow) {
        /* Allocate 3MB RAM shadow buffer (768 4KB frames) */
        uint32_t shadow_phys = pmm_alloc_contiguous_frames(768);
        if (!shadow_phys) {
            serial_printf("[fb_text] Failed to allocate RAM shadow buffer frames\n");
            return;
        }
        fb_shadow = (uint32_t*) P2V(shadow_phys);
    }

    uint32_t bg_rgb = vga_to_rgb32[(terminal_color >> 4) & 0x0F];
    for (size_t i = 0; i < 1024 * 768; i++) {
        fb_shadow[i] = bg_rgb;
    }

    for (size_t r = 0; r < FB_ROWS; r++) {
        for (size_t c = 0; c < FB_COLS; c++) {
            fb_cells[r][c].c = ' ';
            fb_cells[r][c].color = terminal_color;
        }
    }

    fb_cursor_col = 0;
    fb_cursor_row = 0;
    fb_cursor_visible = 1;
    fb_draw_cell_glyph(0, 0, ' ', terminal_color, 1);

    if (g_kd_mode == KD_TEXT && g_fb_info.virt_addr) {
        memcpy(g_fb_info.virt_addr, fb_shadow, 1024 * 768 * sizeof(uint32_t));
    }
}

static void fb_backend_putc(size_t col, size_t row, unsigned char c, uint8_t color) {
    if (col >= FB_COLS || row >= FB_ROWS || !fb_shadow) return;

    fb_cells[row][col].c = c;
    fb_cells[row][col].color = color;

    int is_cur = (fb_cursor_visible && col == fb_cursor_col && row == fb_cursor_row);
    fb_draw_cell_glyph(col, row, c, color, is_cur);

    fb_flush_rows(row, row);
}

static void fb_backend_scroll(uint8_t color) {
    if (!fb_shadow) return;

    /* 1. Shift RAM shadow buffer up by 16 pixel rows */
    memmove(fb_shadow, fb_shadow + 16 * 1024, (768 - 16) * 1024 * sizeof(uint32_t));

    /* 2. Clear bottom 16 pixel rows */
    uint32_t bg_rgb = vga_to_rgb32[(color >> 4) & 0x0F];
    for (size_t i = (768 - 16) * 1024; i < 768 * 1024; i++) {
        fb_shadow[i] = bg_rgb;
    }

    /* 3. Shift cell metadata */
    memmove(&fb_cells[0][0], &fb_cells[1][0], (FB_ROWS - 1) * FB_COLS * sizeof(struct fb_cell));
    for (size_t c = 0; c < FB_COLS; c++) {
        fb_cells[FB_ROWS - 1][c].c = ' ';
        fb_cells[FB_ROWS - 1][c].color = color;
    }

    /* 4. Flush entire screen to hardware framebuffer without reading VRAM */
    if (g_kd_mode == KD_TEXT && g_fb_info.virt_addr) {
        memcpy(g_fb_info.virt_addr, fb_shadow, 1024 * 768 * sizeof(uint32_t));
    }
}

static void fb_backend_set_cursor(size_t col, size_t row) {
    if (!fb_shadow) return;

    /* Hide cursor at old position */
    if (fb_cursor_row < FB_ROWS && fb_cursor_col < FB_COLS) {
        fb_draw_cell_glyph(fb_cursor_col, fb_cursor_row,
                           fb_cells[fb_cursor_row][fb_cursor_col].c,
                           fb_cells[fb_cursor_row][fb_cursor_col].color, 0);
        fb_flush_rows(fb_cursor_row, fb_cursor_row);
    }

    fb_cursor_col = col;
    fb_cursor_row = row;

    /* Show cursor at new position */
    if (fb_cursor_row < FB_ROWS && fb_cursor_col < FB_COLS && fb_cursor_visible) {
        fb_draw_cell_glyph(fb_cursor_col, fb_cursor_row,
                           fb_cells[fb_cursor_row][fb_cursor_col].c,
                           fb_cells[fb_cursor_row][fb_cursor_col].color, 1);
        fb_flush_rows(fb_cursor_row, fb_cursor_row);
    }
}

static void fb_backend_clear(uint8_t color) {
    if (!fb_shadow) return;

    uint32_t bg_rgb = vga_to_rgb32[(color >> 4) & 0x0F];
    for (size_t i = 0; i < 1024 * 768; i++) {
        fb_shadow[i] = bg_rgb;
    }

    for (size_t r = 0; r < FB_ROWS; r++) {
        for (size_t c = 0; c < FB_COLS; c++) {
            fb_cells[r][c].c = ' ';
            fb_cells[r][c].color = color;
        }
    }

    if (g_kd_mode == KD_TEXT && g_fb_info.virt_addr) {
        memcpy(g_fb_info.virt_addr, fb_shadow, 1024 * 768 * sizeof(uint32_t));
    }
}

static void fb_backend_set_color(uint8_t color) {
    (void) color;
}

static struct console_ops fb_text_ops = {
    .name = "fb_text",
    .cols = FB_COLS,
    .rows = FB_ROWS,
    .init = fb_backend_init,
    .putc = fb_backend_putc,
    .scroll = fb_backend_scroll,
    .set_cursor = fb_backend_set_cursor,
    .clear = fb_backend_clear,
    .set_color = fb_backend_set_color,
};

/* ========================================================================= */
/*                   SWITCH TO FRAMEBUFFER CONSOLE ON BOOT                   */
/* ========================================================================= */

void console_switch_to_fb(void) {
    if (!fb_is_present() || !g_fb_info.virt_addr) return;

    /* Initialize fb_text */
    fb_backend_init();

    /* Copy existing VGA text lines to fb_text */
    if (vga_buffer) {
        for (size_t r = 0; r <= terminal_row && r < 25 && r < FB_ROWS; r++) {
            size_t max_c = (r == terminal_row) ? terminal_column : 80;
            if (max_c > 80) max_c = 80;
            for (size_t c = 0; c < max_c && c < FB_COLS; c++) {
                uint16_t entry = vga_buffer[r * 80 + c];
                unsigned char ch = (unsigned char)(entry & 0xFF);
                uint8_t clr = (uint8_t)((entry >> 8) & 0xFF);
                if (ch != 0) {
                    fb_cells[r][c].c = ch;
                    fb_cells[r][c].color = clr;
                    fb_draw_cell_glyph(c, r, ch, clr, 0);
                }
            }
        }
    }

    current_ops = &fb_text_ops;

    /* Update cursor and flush full screen */
    if (current_ops->set_cursor) {
        current_ops->set_cursor(terminal_column, terminal_row);
    }

    if (g_kd_mode == KD_TEXT && g_fb_info.virt_addr && fb_shadow) {
        memcpy(g_fb_info.virt_addr, fb_shadow, 1024 * 768 * sizeof(uint32_t));
    }

    serial_printf("[console] Switched to graphical text console (128x48, PSF2 Terminus)\n");
}

/* ========================================================================= */
/*                             MODE SWITCHING                                */
/* ========================================================================= */

int console_set_mode(int mode) {
    if (mode == KD_GRAPHICS) {
        g_kd_mode = KD_GRAPHICS;
        process_t* cur = scheduler_current();
        if (cur) g_graphics_pid = cur->pid;
        return 0;
    } else if (mode == KD_TEXT) {
        g_kd_mode = KD_TEXT;
        g_graphics_pid = 0;
        console_repaint();
        return 0;
    }
    return -EINVAL;
}

int console_get_mode(void) {
    return g_kd_mode;
}

void console_restore_text_mode(void) {
    if (g_kd_mode == KD_GRAPHICS) {
        g_kd_mode = KD_TEXT;
        g_graphics_pid = 0;
        console_repaint();
    }
    g_kb_mode = K_XLATE;
}

void console_on_process_exit(int pid) {
    if (g_kd_mode == KD_GRAPHICS && (g_graphics_pid == 0 || g_graphics_pid == pid)) {
        console_restore_text_mode();
    }
}

void console_repaint(void) {
    if (g_kd_mode != KD_TEXT) return;

    if (current_ops == &fb_text_ops && fb_shadow && g_fb_info.virt_addr) {
        if (fb_cursor_row < FB_ROWS && fb_cursor_col < FB_COLS && fb_cursor_visible) {
            fb_draw_cell_glyph(fb_cursor_col, fb_cursor_row,
                               fb_cells[fb_cursor_row][fb_cursor_col].c,
                               fb_cells[fb_cursor_row][fb_cursor_col].color, 1);
        }
        memcpy(g_fb_info.virt_addr, fb_shadow, 1024 * 768 * sizeof(uint32_t));
    } else if (current_ops == &vga_text_ops) {
        vga_update_hw_cursor(terminal_column, terminal_row);
    }
}

int console_set_kbmode(int mode) {
    if (mode != K_XLATE && mode != K_OFF && mode != K_RAW) return -EINVAL;
    g_kb_mode = mode;
    return 0;
}

int console_get_kbmode(void) {
    return g_kb_mode;
}

size_t console_get_cols(void) {
    return current_ops ? current_ops->cols : 80;
}

size_t console_get_rows(void) {
    return current_ops ? current_ops->rows : 25;
}

/* ========================================================================= */
/*                          ANSI PARSER & TERMINAL API                       */
/* ========================================================================= */

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

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    fg_color = VGA_LIGHT_GREY;
    bg_color = VGA_BLACK;
    sgr_bold = 0;
    sgr_reverse = 0;
    update_terminal_color();

    current_ops = &vga_text_ops;
    current_ops->init();
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
    if (current_ops && current_ops->set_color) {
        current_ops->set_color(color);
    }
}

static void handle_sgr(int param) {
    if (param == 0) {
        /* Reset */
        fg_color = VGA_LIGHT_GREY;
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
        fg_color = VGA_LIGHT_GREY;
    } else if (param >= 40 && param <= 47) {
        bg_color = ansi_color_map[param - 40];
    } else if (param == 49) {
        bg_color = VGA_BLACK;
    }
    update_terminal_color();
}

void terminal_putchar(char c) {
    size_t cols = current_ops->cols;
    size_t rows = current_ops->rows;

    if (parser_state == ANSI_STATE_NORMAL) {
        if (c == 0x1B) { /* ESC */
            parser_state = ANSI_STATE_ESC;
            return;
        }

        if (c == '\r') {
            terminal_column = 0;
            current_ops->set_cursor(terminal_column, terminal_row);
            return;
        }

        if (c == '\n') {
            terminal_column = 0;
            if (++terminal_row == rows) {
                current_ops->scroll(terminal_color);
                terminal_row = rows - 1;
            }
            current_ops->set_cursor(terminal_column, terminal_row);
            return;
        }

        if (c == '\b') {
            if (terminal_column > 0) {
                terminal_column--;
            }
            current_ops->set_cursor(terminal_column, terminal_row);
            return;
        }

        if (c == '\t') {
            terminal_column = (terminal_column + 8) & ~7;
            if (terminal_column >= cols) {
                terminal_column = 0;
                if (++terminal_row == rows) {
                    current_ops->scroll(terminal_color);
                    terminal_row = rows - 1;
                }
            }
            current_ops->set_cursor(terminal_column, terminal_row);
            return;
        }

        if ((unsigned char)c >= 32) {
            current_ops->putc(terminal_column, terminal_row, (unsigned char)c, terminal_color);
            if (++terminal_column == cols) {
                terminal_column = 0;
                if (++terminal_row == rows) {
                    current_ops->scroll(terminal_color);
                    terminal_row = rows - 1;
                }
            }
            current_ops->set_cursor(terminal_column, terminal_row);
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
                if (terminal_row >= rows) terminal_row = rows - 1;
                break;
            }
            case 'C': { /* Cursor Forward */
                int n = (csi_param_count > 0 && csi_params[0] > 0) ? csi_params[0] : 1;
                terminal_column += n;
                if (terminal_column >= cols) terminal_column = cols - 1;
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
                if (r >= (int)rows) r = rows - 1;
                if (col < 0) col = 0;
                if (col >= (int)cols) col = cols - 1;
                terminal_row = r;
                terminal_column = col;
                break;
            }
            case 'J': { /* Erase Display */
                int mode = (csi_param_count > 0) ? csi_params[0] : 0;
                if (mode == 2) {
                    current_ops->clear(terminal_color);
                    terminal_row = 0;
                    terminal_column = 0;
                } else if (mode == 0) {
                    for (size_t x = terminal_column; x < cols; x++)
                        current_ops->putc(x, terminal_row, ' ', terminal_color);
                    for (size_t y = terminal_row + 1; y < rows; y++)
                        for (size_t x = 0; x < cols; x++)
                            current_ops->putc(x, y, ' ', terminal_color);
                } else if (mode == 1) {
                    for (size_t y = 0; y < terminal_row; y++)
                        for (size_t x = 0; x < cols; x++)
                            current_ops->putc(x, y, ' ', terminal_color);
                    for (size_t x = 0; x <= terminal_column; x++)
                        current_ops->putc(x, terminal_row, ' ', terminal_color);
                }
                break;
            }
            case 'K': { /* Erase in Line */
                int mode = (csi_param_count > 0) ? csi_params[0] : 0;
                if (mode == 0) {
                    for (size_t x = terminal_column; x < cols; x++)
                        current_ops->putc(x, terminal_row, ' ', terminal_color);
                } else if (mode == 1) {
                    for (size_t x = 0; x <= terminal_column; x++)
                        current_ops->putc(x, terminal_row, ' ', terminal_color);
                } else if (mode == 2) {
                    for (size_t x = 0; x < cols; x++)
                        current_ops->putc(x, terminal_row, ' ', terminal_color);
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
        current_ops->set_cursor(terminal_column, terminal_row);
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

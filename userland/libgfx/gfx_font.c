#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* --- Embedded 8x16 Bitmap Font --- */
/* Standard 8x16 IBM VGA / PSF font glyph table for ASCII 32..126 */
extern const uint8_t g_embedded_font8x16[128][16];

void gfx_draw_char(gfx_surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!s || !s->pixels) return;
    unsigned char uc = (unsigned char) c;
    if (uc > 127) uc = '?';

    const uint8_t *glyph = g_embedded_font8x16[uc];

    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < s->clip.y0 || py >= s->clip.y1) continue;

        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < s->clip.x0 || px >= s->clip.x1) continue;

            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            gfx_putpixel(s, px, py, color);
        }
    }
}

void gfx_draw_text(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg) {
    if (!s || !str) return;
    int cx = x;
    int cy = y;

    while (*str) {
        if (*str == '\n') {
            cy += 16;
            cx = x;
        } else if (*str == '\t') {
            cx += 8 * 4;
        } else {
            gfx_draw_char(s, cx, cy, *str, fg, bg);
            cx += 8;
        }
        str++;
    }
}

void gfx_draw_text_transparent(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg) {
    if (!s || !str) return;
    int cx = x;
    int cy = y;

    while (*str) {
        if (*str == '\n') {
            cy += 16;
            cx = x;
        } else if (*str == '\t') {
            cx += 8 * 4;
        } else {
            unsigned char uc = (unsigned char) *str;
            if (uc > 127) uc = '?';
            const uint8_t *glyph = g_embedded_font8x16[uc];

            for (int row = 0; row < 16; row++) {
                int py = cy + row;
                if (py < s->clip.y0 || py >= s->clip.y1) continue;
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    int px = cx + col;
                    if (px < s->clip.x0 || px >= s->clip.x1) continue;
                    if (bits & (0x80 >> col)) {
                        gfx_putpixel(s, px, py, fg);
                    }
                }
            }
            cx += 8;
        }
        str++;
    }
}

/* --- TrueType Font Subsystem --- */

struct gfx_font {
    unsigned char *ttf_buffer;
    stbtt_fontinfo info;
    float size;
    float scale;
    int ascent;
    int descent;
    int line_gap;
};

gfx_font_t *gfx_load_font(const char *path, float pixel_height) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0) {
        close(fd);
        return NULL;
    }

    unsigned char *buf = (unsigned char *) malloc((size_t) size);
    if (!buf) {
        close(fd);
        return NULL;
    }

    ssize_t rd = read(fd, buf, (size_t) size);
    close(fd);
    if (rd != (ssize_t) size) {
        free(buf);
        return NULL;
    }

    gfx_font_t *font = (gfx_font_t *) calloc(1, sizeof(gfx_font_t));
    if (!font) {
        free(buf);
        return NULL;
    }

    font->ttf_buffer = buf;
    font->size = pixel_height;

    if (!stbtt_InitFont(&font->info, buf, 0)) {
        free(buf);
        free(font);
        return NULL;
    }

    font->scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);

    return font;
}

void gfx_destroy_font(gfx_font_t *font) {
    if (!font) return;
    if (font->ttf_buffer) {
        free(font->ttf_buffer);
        font->ttf_buffer = NULL;
    }
    free(font);
}

int gfx_measure_text_ttf(gfx_font_t *font, const char *str, int *out_w, int *out_h) {
    if (!font || !str) return 0;

    int w = 0;
    while (*str) {
        int ax, lsb;
        stbtt_GetCodepointHMetrics(&font->info, *str, &ax, &lsb);
        w += (int) (ax * font->scale);
        str++;
    }

    int h = (int) ((font->ascent - font->descent) * font->scale);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return w;
}

void gfx_draw_text_ttf(gfx_surface_t *s, gfx_font_t *font, int x, int y, const char *str, uint32_t color) {
    if (!s || !font || !str) return;

    int baseline = y + (int) (font->ascent * font->scale);
    int cx = x;

    uint32_t cr = GFX_GET_R(color);
    uint32_t cg = GFX_GET_G(color);
    uint32_t cb = GFX_GET_B(color);

    while (*str) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font->info, *str, &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&font->info, *str, font->scale, font->scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int gw = c_x2 - c_x1;
        int gh = c_y2 - c_y1;

        if (gw > 0 && gh > 0) {
            unsigned char *bitmap = (unsigned char *) calloc((size_t)(gw * gh), 1);
            if (bitmap) {
                stbtt_MakeCodepointBitmap(&font->info, bitmap, gw, gh, gw, font->scale, font->scale, *str);

                int draw_x = cx + c_x1;
                int draw_y = baseline + c_y1;

                for (int row = 0; row < gh; row++) {
                    int py = draw_y + row;
                    if (py < s->clip.y0 || py >= s->clip.y1) continue;

                    for (int col = 0; col < gw; col++) {
                        int px = draw_x + col;
                        if (px < s->clip.x0 || px >= s->clip.x1) continue;

                        unsigned char alpha = bitmap[row * gw + col];
                        if (alpha > 0) {
                            uint32_t pixel_color = GFX_RGBA(cr, cg, cb, alpha);
                            gfx_putpixel_alpha(s, px, py, pixel_color);
                        }
                    }
                }
                free(bitmap);
            }
        }

        cx += (int) (advance * font->scale);
        str++;
    }
}

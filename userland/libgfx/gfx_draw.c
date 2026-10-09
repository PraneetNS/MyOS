#include "gfx.h"
#include <stdlib.h>
#include <string.h>

void gfx_clear(gfx_surface_t *s, uint32_t color) {
    if (!s || !s->pixels) return;
    int count = s->width * s->height;
    for (int i = 0; i < count; i++) {
        s->pixels[i] = color;
    }
}

static inline int point_in_clip(const gfx_surface_t *s, int x, int y) {
    return (x >= s->clip.x0 && x < s->clip.x1 && y >= s->clip.y0 && y < s->clip.y1);
}

void gfx_putpixel(gfx_surface_t *s, int x, int y, uint32_t color) {
    if (!s || !s->pixels) return;
    if (point_in_clip(s, x, y)) {
        int pitch_words = s->pitch / sizeof(uint32_t);
        s->pixels[y * pitch_words + x] = color;
    }
}

uint32_t gfx_getpixel(const gfx_surface_t *s, int x, int y) {
    if (!s || !s->pixels) return 0;
    if (x >= 0 && x < s->width && y >= 0 && y < s->height) {
        int pitch_words = s->pitch / sizeof(uint32_t);
        return s->pixels[y * pitch_words + x];
    }
    return 0;
}

uint32_t gfx_blend(uint32_t dst, uint32_t src) {
    uint32_t a = GFX_GET_A(src);
    if (a == 0) return dst;
    if (a == 255) return src;

    uint32_t inv_a = 255 - a;

    uint32_t sr = GFX_GET_R(src);
    uint32_t sg = GFX_GET_G(src);
    uint32_t sb = GFX_GET_B(src);

    uint32_t dr = GFX_GET_R(dst);
    uint32_t dg = GFX_GET_G(dst);
    uint32_t db = GFX_GET_B(dst);

    uint32_t r = (sr * a + dr * inv_a) / 255;
    uint32_t g = (sg * a + dg * inv_a) / 255;
    uint32_t b = (sb * a + db * inv_a) / 255;

    return GFX_RGB(r, g, b);
}

void gfx_putpixel_alpha(gfx_surface_t *s, int x, int y, uint32_t color) {
    if (!s || !s->pixels) return;
    if (point_in_clip(s, x, y)) {
        int pitch_words = s->pitch / sizeof(uint32_t);
        uint32_t *p = &s->pixels[y * pitch_words + x];
        *p = gfx_blend(*p, color);
    }
}

void gfx_draw_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, uint32_t color) {
    if (!s) return;

    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        gfx_putpixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_draw_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || w <= 0 || h <= 0) return;
    gfx_draw_line(s, x, y, x + w - 1, y, color);
    gfx_draw_line(s, x, y + h - 1, x + w - 1, y + h - 1, color);
    gfx_draw_line(s, x, y, x, y + h - 1, color);
    gfx_draw_line(s, x + w - 1, y, x + w - 1, y + h - 1, color);
}

void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels || w <= 0 || h <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < s->clip.x0) x0 = s->clip.x0;
    if (y0 < s->clip.y0) y0 = s->clip.y0;
    if (x1 > s->clip.x1) x1 = s->clip.x1;
    if (y1 > s->clip.y1) y1 = s->clip.y1;

    if (x0 >= x1 || y0 >= y1) return;

    int pitch_words = s->pitch / sizeof(uint32_t);
    int span_w = x1 - x0;

    for (int cy = y0; cy < y1; cy++) {
        uint32_t *row = s->pixels + cy * pitch_words + x0;
        for (int cx = 0; cx < span_w; cx++) {
            row[cx] = color;
        }
    }
}

void gfx_fill_rect_alpha(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->pixels || w <= 0 || h <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < s->clip.x0) x0 = s->clip.x0;
    if (y0 < s->clip.y0) y0 = s->clip.y0;
    if (x1 > s->clip.x1) x1 = s->clip.x1;
    if (y1 > s->clip.y1) y1 = s->clip.y1;

    if (x0 >= x1 || y0 >= y1) return;

    int pitch_words = s->pitch / sizeof(uint32_t);
    int span_w = x1 - x0;

    for (int cy = y0; cy < y1; cy++) {
        uint32_t *row = s->pixels + cy * pitch_words + x0;
        for (int cx = 0; cx < span_w; cx++) {
            row[cx] = gfx_blend(row[cx], color);
        }
    }
}

void gfx_draw_circle(gfx_surface_t *s, int xc, int yc, int r, uint32_t color) {
    if (!s || r <= 0) return;

    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        gfx_putpixel(s, xc + x, yc + y, color);
        gfx_putpixel(s, xc - x, yc + y, color);
        gfx_putpixel(s, xc + x, yc - y, color);
        gfx_putpixel(s, xc - x, yc - y, color);
        gfx_putpixel(s, xc + y, yc + x, color);
        gfx_putpixel(s, xc - y, yc + x, color);
        gfx_putpixel(s, xc + y, yc - x, color);
        gfx_putpixel(s, xc - y, yc - x, color);

        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

static void draw_hline(gfx_surface_t *s, int x0, int x1, int y, uint32_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y < s->clip.y0 || y >= s->clip.y1) return;
    if (x0 < s->clip.x0) x0 = s->clip.x0;
    if (x1 >= s->clip.x1) x1 = s->clip.x1 - 1;
    if (x0 > x1) return;

    int pitch_words = s->pitch / sizeof(uint32_t);
    uint32_t *row = s->pixels + y * pitch_words;
    for (int x = x0; x <= x1; x++) {
        row[x] = color;
    }
}

void gfx_fill_circle(gfx_surface_t *s, int xc, int yc, int r, uint32_t color) {
    if (!s || r <= 0) return;

    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        draw_hline(s, xc - x, xc + x, yc + y, color);
        draw_hline(s, xc - x, xc + x, yc - y, color);
        draw_hline(s, xc - y, xc + y, yc + x, color);
        draw_hline(s, xc - y, xc + y, yc - x, color);

        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

void gfx_blit(gfx_surface_t *dst, int dx, int dy, const gfx_surface_t *src, int sx, int sy, int sw, int sh) {
    if (!dst || !src || sw <= 0 || sh <= 0) return;

    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (sx + sw > src->width) sw = src->width - sx;
    if (sy + sh > src->height) sh = src->height - sy;

    int dst_x0 = dx;
    int dst_y0 = dy;
    int dst_x1 = dx + sw;
    int dst_y1 = dy + sh;

    if (dst_x0 < dst->clip.x0) { sx += (dst->clip.x0 - dst_x0); dst_x0 = dst->clip.x0; }
    if (dst_y0 < dst->clip.y0) { sy += (dst->clip.y0 - dst_y0); dst_y0 = dst->clip.y0; }
    if (dst_x1 > dst->clip.x1) dst_x1 = dst->clip.x1;
    if (dst_y1 > dst->clip.y1) dst_y1 = dst->clip.y1;

    int w = dst_x1 - dst_x0;
    int h = dst_y1 - dst_y0;
    if (w <= 0 || h <= 0) return;

    int dst_pitch_words = dst->pitch / sizeof(uint32_t);
    int src_pitch_words = src->pitch / sizeof(uint32_t);
    int copy_bytes = w * sizeof(uint32_t);

    for (int r = 0; r < h; r++) {
        uint32_t *drow = dst->pixels + (dst_y0 + r) * dst_pitch_words + dst_x0;
        const uint32_t *srow = src->pixels + (sy + r) * src_pitch_words + sx;
        memcpy(drow, srow, copy_bytes);
    }
}

void gfx_blit_alpha(gfx_surface_t *dst, int dx, int dy, const gfx_surface_t *src, int sx, int sy, int sw, int sh) {
    if (!dst || !src || sw <= 0 || sh <= 0) return;

    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (sx + sw > src->width) sw = src->width - sx;
    if (sy + sh > src->height) sh = src->height - sy;

    int dst_x0 = dx;
    int dst_y0 = dy;
    int dst_x1 = dx + sw;
    int dst_y1 = dy + sh;

    if (dst_x0 < dst->clip.x0) { sx += (dst->clip.x0 - dst_x0); dst_x0 = dst->clip.x0; }
    if (dst_y0 < dst->clip.y0) { sy += (dst->clip.y0 - dst_y0); dst_y0 = dst->clip.y0; }
    if (dst_x1 > dst->clip.x1) dst_x1 = dst->clip.x1;
    if (dst_y1 > dst->clip.y1) dst_y1 = dst->clip.y1;

    int w = dst_x1 - dst_x0;
    int h = dst_y1 - dst_y0;
    if (w <= 0 || h <= 0) return;

    int dst_pitch_words = dst->pitch / sizeof(uint32_t);
    int src_pitch_words = src->pitch / sizeof(uint32_t);

    for (int r = 0; r < h; r++) {
        uint32_t *drow = dst->pixels + (dst_y0 + r) * dst_pitch_words + dst_x0;
        const uint32_t *srow = src->pixels + (sy + r) * src_pitch_words + sx;
        for (int c = 0; c < w; c++) {
            drow[c] = gfx_blend(drow[c], srow[c]);
        }
    }
}

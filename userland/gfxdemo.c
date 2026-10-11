#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include "gfx.h"

#define NUM_BALLS 6

typedef struct {
    float x, y;
    float vx, vy;
    int radius;
    uint32_t color;
} ball_t;

static const uint32_t PALETTE[16] = {
    GFX_RGB(15, 23, 42),    /* 0: Slate 900 */
    GFX_RGB(239, 68, 68),   /* 1: Red 500 */
    GFX_RGB(249, 115, 22),  /* 2: Orange 500 */
    GFX_RGB(245, 158, 11),  /* 3: Amber 500 */
    GFX_RGB(16, 185, 129),  /* 4: Emerald 500 */
    GFX_RGB(20, 184, 166),  /* 5: Teal 500 */
    GFX_RGB(6, 182, 212),   /* 6: Cyan 500 */
    GFX_RGB(59, 130, 246),  /* 7: Blue 500 */
    GFX_RGB(99, 102, 241),  /* 8: Indigo 500 */
    GFX_RGB(139, 92, 246),  /* 9: Violet 500 */
    GFX_RGB(217, 70, 239),  /* 10: Fuchsia 500 */
    GFX_RGB(244, 63, 94),   /* 11: Rose 500 */
    GFX_RGB(100, 116, 139), /* 12: Slate 500 */
    GFX_RGB(203, 213, 225), /* 13: Slate 300 */
    GFX_RGB(255, 255, 255), /* 14: White */
    GFX_RGB(234, 179, 8),   /* 15: Yellow 500 */
};

static gfx_context_t *g_ctx = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_ctx) {
        gfx_close(g_ctx);
        g_ctx = NULL;
    }
    _exit(0);
}

static void draw_background(gfx_surface_t *s, int w, int h) {
    for (int y = 0; y < h; y++) {
        uint8_t r = 15 + (y * 10) / h;
        uint8_t g = 20 + (y * 12) / h;
        uint8_t b = 30 + (y * 15) / h;
        uint32_t col = GFX_RGB(r, g, b);
        for (int x = 0; x < w; x++) {
            s->pixels[y * (s->pitch / 4) + x] = col;
        }
    }
}

static void draw_header(gfx_surface_t *s, int w, gfx_font_t *ttf_font) {
    gfx_fill_rect(s, 0, 0, w, 40, GFX_RGB(24, 32, 47));
    gfx_draw_line(s, 0, 40, w, 40, GFX_RGB(51, 65, 85));

    if (ttf_font) {
        gfx_draw_text_ttf(s, ttf_font, 16, 8, "MyOS 2D Graphics Subsystem", GFX_RGB(248, 250, 252));
    } else {
        gfx_draw_text(s, 16, 12, "MyOS 2D Graphics Subsystem (libgfx)", GFX_RGB(248, 250, 252), GFX_RGB(24, 32, 47));
    }
    gfx_draw_text(s, w - 240, 12, "Stage 18A Demo | 1024x768", GFX_RGB(148, 163, 184), GFX_RGB(24, 32, 47));
}

static void draw_deterministic_squares(gfx_surface_t *s) {
    /* Exact reference test patches for automated pixel inspection:
     * Red:   (50,50)  to (150,150) -> width 100, height 100, exact 0xFF0000
     * Green: (200,50) to (300,150) -> width 100, height 100, exact 0x00FF00
     * Blue:  (350,50) to (450,150) -> width 100, height 100, exact 0x0000FF
     * No alpha, no anti-aliasing.
     */
    gfx_fill_rect(s, 50, 50, 100, 100, GFX_RGB(255, 0, 0));
    gfx_fill_rect(s, 200, 50, 100, 100, GFX_RGB(0, 255, 0));
    gfx_fill_rect(s, 350, 50, 100, 100, GFX_RGB(0, 0, 255));
}

static void draw_palette_swatches(gfx_surface_t *s, int x, int y) {
    gfx_draw_text(s, x, y, "Vibrant Palette Swatches (16-Color)", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));
    y += 20;

    int swatch_w = 26;
    int swatch_h = 22;
    for (int i = 0; i < 16; i++) {
        int sx = x + i * (swatch_w + 4);
        gfx_fill_rect(s, sx, y, swatch_w, swatch_h, PALETTE[i]);
        gfx_draw_rect(s, sx, y, swatch_w, swatch_h, GFX_RGB(255, 255, 255));
    }
}

static void draw_geometry_showcase(gfx_surface_t *s, int x, int y) {
    gfx_draw_text(s, x, y, "2D Geometry & Bresenham Primitives", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));
    y += 20;

    gfx_fill_rect(s, x, y, 460, 180, GFX_RGB(30, 41, 59));
    gfx_draw_rect(s, x, y, 460, 180, GFX_RGB(71, 85, 105));

    /* Starburst radial lines */
    int cx = x + 75;
    int cy = y + 90;
    for (int deg = 0; deg < 360; deg += 30) {
        float rad = (float)deg * 3.14159265f / 180.0f;
        int ex = cx + (int)(cosf(rad) * 50.0f);
        int ey = cy + (int)(sinf(rad) * 50.0f);
        gfx_draw_line(s, cx, cy, ex, ey, PALETTE[deg / 24]);
    }

    /* Outlined and filled rectangles */
    int rx = x + 165;
    int ry = y + 25;
    gfx_draw_rect(s, rx, ry, 110, 55, GFX_RGB(6, 182, 212));
    gfx_draw_rect(s, rx + 10, ry + 10, 90, 35, GFX_RGB(20, 184, 166));
    gfx_fill_rect(s, rx, ry + 70, 110, 55, GFX_RGB(99, 102, 241));
    gfx_draw_rect(s, rx, ry + 70, 110, 55, GFX_RGB(199, 210, 254));

    /* Circles */
    int kx = x + 370;
    int ky = y + 60;
    gfx_draw_circle(s, kx, ky, 35, GFX_RGB(244, 63, 94));
    gfx_draw_circle(s, kx, ky, 20, GFX_RGB(251, 146, 60));
    gfx_fill_circle(s, kx, ky + 70, 25, GFX_RGB(16, 185, 129));
    gfx_draw_circle(s, kx, ky + 70, 25, GFX_RGB(255, 255, 255));
}

static void draw_alpha_showcase(gfx_surface_t *s, int x, int y) {
    gfx_draw_text(s, x, y, "Alpha Blending & Compositing (RGBA)", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));
    y += 20;

    gfx_fill_rect(s, x, y, 460, 180, GFX_RGB(20, 25, 35));
    gfx_draw_rect(s, x, y, 460, 180, GFX_RGB(71, 85, 105));

    int grid_size = 16;
    for (int gy = y + 4; gy < y + 176; gy += grid_size) {
        for (int gx = x + 4; gx < x + 456; gx += grid_size) {
            if (((gx / grid_size) + (gy / grid_size)) % 2 == 0) {
                gfx_fill_rect(s, gx, gy, grid_size, grid_size, GFX_RGB(35, 45, 60));
            }
        }
    }

    /* Overlapping translucent rectangles with alpha */
    gfx_fill_rect_alpha(s, x + 35, y + 30, 120, 100, GFX_RGBA(239, 68, 68, 160));
    gfx_fill_rect_alpha(s, x + 95, y + 45, 120, 100, GFX_RGBA(59, 130, 246, 160));
    gfx_fill_rect_alpha(s, x + 65, y + 70, 120, 80, GFX_RGBA(16, 185, 129, 160));

    /* Alpha circle overlays */
    for (int r = 35; r >= 10; r -= 10) {
        gfx_draw_circle(s, x + 350, y + 90, r, GFX_RGB(245, 158, 11));
    }
    gfx_fill_rect_alpha(s, x + 300, y + 45, 90, 90, GFX_RGBA(217, 70, 239, 140));
}

static void draw_typography_showcase(gfx_surface_t *s, int x, int y, gfx_font_t *ttf_font) {
    gfx_draw_text(s, x, y, "Typography: Embedded 8x16 Bitmap + TrueType (stb_truetype)", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));
    y += 18;

    gfx_fill_rect(s, x, y, 940, 108, GFX_RGB(30, 41, 59));
    gfx_draw_rect(s, x, y, 940, 108, GFX_RGB(71, 85, 105));

    gfx_draw_text(s, x + 16, y + 10, "[Bitmap Font 8x16] The quick brown fox jumps over the lazy dog. 0123456789",
                  GFX_RGB(147, 197, 253), GFX_RGB(30, 41, 59));
    gfx_draw_text(s, x + 16, y + 28, "[Bitmap Font 8x16] ASCII !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
                  GFX_RGB(253, 186, 116), GFX_RGB(30, 41, 59));

    if (ttf_font) {
        gfx_draw_text(s, x + 16, y + 48, "[TrueType DejaVuSans 22pt Anti-Aliased]:", GFX_RGB(148, 163, 184), GFX_RGB(30, 41, 59));
        gfx_draw_text_ttf(s, ttf_font, x + 16, y + 68, "Sphinx of black quartz, judge my vow!", GFX_RGB(255, 255, 255));
        gfx_draw_text_ttf(s, ttf_font, x + 16, y + 88, "MyOS Unix-like Kernel with PAT Write-Combining", GFX_RGB(167, 139, 250));
    } else {
        gfx_draw_text(s, x + 16, y + 55, "[TrueType Font not found at /share/fonts/dejavu.ttf]", GFX_RGB(239, 68, 68), GFX_RGB(30, 41, 59));
    }
}

static void draw_footer(gfx_surface_t *s, int w, int h) {
    int y = h - 28;
    gfx_fill_rect(s, 0, y, w, 28, GFX_RGB(15, 23, 42));
    gfx_draw_line(s, 0, y, w, y, GFX_RGB(51, 65, 85));
    gfx_draw_text(s, 16, y + 6, "Press ESC or Q to return to shell | Drag mouse to move cursor", GFX_RGB(148, 163, 184), GFX_RGB(15, 23, 42));
}

int main(int argc, char *argv[]) {
    int still_mode = 0;
    int max_frames = 0;
    int timeout_ms = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--still") == 0) {
            still_mode = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);

    gfx_context_t *ctx = gfx_open();
    if (!ctx) {
        fprintf(stderr, "gfxdemo: failed to open graphics context\n");
        return 1;
    }
    g_ctx = ctx;

    /* Load TrueType font */
    gfx_font_t *ttf_font = gfx_load_font("/share/fonts/dejavu.ttf", 22.0f);

    /* --- STILL MODE (Deterministic anchor for pixel testing) --- */
    if (still_mode) {
        draw_background(ctx->backbuffer, ctx->width, ctx->height);
        draw_header(ctx->backbuffer, ctx->width, ttf_font);
        draw_deterministic_squares(ctx->backbuffer);
        draw_palette_swatches(ctx->backbuffer, 480, 50);
        draw_geometry_showcase(ctx->backbuffer, 36, 170);
        draw_alpha_showcase(ctx->backbuffer, 520, 170);
        draw_typography_showcase(ctx->backbuffer, 36, 360, ttf_font);

        /* Static non-animated balls sample at y=640 */
        gfx_draw_text(ctx->backbuffer, 36, 575, "2D Primitives Showcase (Static Frame)", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));
        gfx_fill_circle(ctx->backbuffer, 120, 640, 18, PALETTE[1]);
        gfx_fill_circle(ctx->backbuffer, 260, 640, 22, PALETTE[4]);
        gfx_fill_circle(ctx->backbuffer, 450, 640, 20, PALETTE[7]);
        gfx_fill_circle(ctx->backbuffer, 620, 640, 24, PALETTE[9]);
        gfx_fill_circle(ctx->backbuffer, 780, 640, 16, PALETTE[10]);
        gfx_fill_circle(ctx->backbuffer, 900, 640, 20, PALETTE[15]);

        draw_footer(ctx->backbuffer, ctx->width, ctx->height);

        /* NO cursor drawn in still mode! Flip once! */
        gfx_flip(ctx);

        /* Wait for ESC, Q, key press, or timeout */
        int elapsed = 0;
        int running = 1;
        while (running) {
            gfx_event_t ev;
            while (gfx_poll_event(ctx, &ev)) {
                if (ev.type == GFX_EVENT_KEY_DOWN) {
                    if (ev.key.keycode == 1 || ev.key.keycode == 16 || timeout_ms <= 0) {
                        running = 0;
                        break;
                    }
                }
            }
            if (timeout_ms > 0) {
                elapsed += 20;
                if (elapsed >= timeout_ms) {
                    break;
                }
            }
            usleep(20000);
        }

        if (ttf_font) gfx_destroy_font(ttf_font);
        gfx_close(ctx);
        g_ctx = NULL;
        printf("[gfxdemo] Still mode completed: 1024x768 reference frame rendered\n");
        return 0;
    }

    /* --- INTERACTIVE ANIMATED MODE --- */
    ball_t balls[NUM_BALLS] = {
        { 120.0f, 620.0f,  4.2f,  3.1f, 16, PALETTE[1] },
        { 260.0f, 630.0f, -3.8f,  4.5f, 20, PALETTE[4] },
        { 450.0f, 640.0f,  5.1f, -3.7f, 18, PALETTE[7] },
        { 600.0f, 610.0f, -4.6f,  3.9f, 22, PALETTE[9] },
        { 750.0f, 635.0f,  3.5f, -4.2f, 15, PALETTE[10] },
        { 880.0f, 625.0f, -4.9f, -3.3f, 19, PALETTE[15] },
    };

    int anim_area_y1 = ctx->height - 35;
    int anim_area_x0 = 20;
    int anim_area_x1 = ctx->width - 20;

    int frame_count = 0;
    int running = 1;

    while (running) {
        gfx_event_t ev;
        while (gfx_poll_event(ctx, &ev)) {
            if (ev.type == GFX_EVENT_KEY_DOWN) {
                if (ev.key.keycode == 1 || ev.key.keycode == 16) { /* ESC or Q */
                    running = 0;
                    break;
                }
            }
        }

        if (!running) break;

        draw_background(ctx->backbuffer, ctx->width, ctx->height);
        draw_header(ctx->backbuffer, ctx->width, ttf_font);
        draw_deterministic_squares(ctx->backbuffer);
        draw_palette_swatches(ctx->backbuffer, 480, 50);
        draw_geometry_showcase(ctx->backbuffer, 36, 170);
        draw_alpha_showcase(ctx->backbuffer, 520, 170);
        draw_typography_showcase(ctx->backbuffer, 36, 390, ttf_font);

        gfx_draw_text(ctx->backbuffer, 36, 575, "Real-time Bouncing Balls & Hardware PAT Write-Combining Flipping", GFX_RGB(226, 232, 240), GFX_RGB(20, 26, 38));

        for (int i = 0; i < NUM_BALLS; i++) {
            balls[i].x += balls[i].vx;
            balls[i].y += balls[i].vy;

            if (balls[i].x - balls[i].radius < anim_area_x0) {
                balls[i].x = anim_area_x0 + balls[i].radius;
                balls[i].vx = -balls[i].vx;
            } else if (balls[i].x + balls[i].radius > anim_area_x1) {
                balls[i].x = anim_area_x1 - balls[i].radius;
                balls[i].vx = -balls[i].vx;
            }

            if (balls[i].y - balls[i].radius < 595) {
                balls[i].y = 595 + balls[i].radius;
                balls[i].vy = -balls[i].vy;
            } else if (balls[i].y + balls[i].radius > anim_area_y1) {
                balls[i].y = anim_area_y1 - balls[i].radius;
                balls[i].vy = -balls[i].vy;
            }

            gfx_fill_circle(ctx->backbuffer, (int)balls[i].x, (int)balls[i].y, balls[i].radius, balls[i].color);
            gfx_draw_circle(ctx->backbuffer, (int)balls[i].x, (int)balls[i].y, balls[i].radius, GFX_RGB(255, 255, 255));
        }

        draw_footer(ctx->backbuffer, ctx->width, ctx->height);

        if (ctx->cursor_visible) {
            gfx_draw_cursor(ctx->backbuffer, ctx->mouse_x, ctx->mouse_y);
        }

        gfx_flip(ctx);
        frame_count++;

        if (max_frames > 0 && frame_count >= max_frames) {
            break;
        }

        usleep(16000);
    }

    if (ttf_font) gfx_destroy_font(ttf_font);
    gfx_close(ctx);
    g_ctx = NULL;
    return 0;
}

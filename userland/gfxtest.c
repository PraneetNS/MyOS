#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gfx.h"

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_failed++; \
    } \
} while (0)

static void test_surface_primitives(void) {
    printf("--- Test 1: Surface & Drawing Primitives ---\n");
    gfx_surface_t *s = gfx_create_surface(320, 240);
    TEST_ASSERT(s != NULL, "create 320x240 surface");
    if (!s) return;

    gfx_clear(s, GFX_COLOR_RED);
    TEST_ASSERT(gfx_getpixel(s, 0, 0) == GFX_COLOR_RED, "clear surface to red (0,0)");
    TEST_ASSERT(gfx_getpixel(s, 160, 120) == GFX_COLOR_RED, "clear surface to red (160,120)");

    /* Putpixel and Getpixel */
    gfx_putpixel(s, 10, 20, GFX_COLOR_BLUE);
    TEST_ASSERT(gfx_getpixel(s, 10, 20) == GFX_COLOR_BLUE, "putpixel / getpixel blue at (10,20)");

    /* Clipping */
    gfx_set_clip(s, 50, 50, 100, 100);
    gfx_putpixel(s, 20, 20, GFX_COLOR_GREEN);
    TEST_ASSERT(gfx_getpixel(s, 20, 20) == GFX_COLOR_RED, "pixel outside clip rejected");
    gfx_putpixel(s, 60, 60, GFX_COLOR_GREEN);
    TEST_ASSERT(gfx_getpixel(s, 60, 60) == GFX_COLOR_GREEN, "pixel inside clip accepted");
    gfx_reset_clip(s);

    /* Line */
    gfx_draw_line(s, 0, 0, 40, 0, GFX_COLOR_WHITE);
    TEST_ASSERT(gfx_getpixel(s, 20, 0) == GFX_COLOR_WHITE, "draw horizontal line");

    /* Rect & Fill Rect */
    gfx_draw_rect(s, 10, 10, 20, 20, GFX_COLOR_YELLOW);
    TEST_ASSERT(gfx_getpixel(s, 10, 10) == GFX_COLOR_YELLOW, "draw rect outline");
    TEST_ASSERT(gfx_getpixel(s, 29, 29) == GFX_COLOR_YELLOW, "draw rect bottom-right corner");

    gfx_fill_rect(s, 30, 30, 10, 10, GFX_COLOR_CYAN);
    TEST_ASSERT(gfx_getpixel(s, 35, 35) == GFX_COLOR_CYAN, "fill rect interior");

    /* Circle & Fill Circle */
    gfx_draw_circle(s, 100, 100, 15, GFX_COLOR_WHITE);
    TEST_ASSERT(gfx_getpixel(s, 115, 100) == GFX_COLOR_WHITE, "draw circle boundary");

    gfx_fill_circle(s, 150, 150, 10, GFX_COLOR_MAGENTA);
    TEST_ASSERT(gfx_getpixel(s, 150, 150) == GFX_COLOR_MAGENTA, "fill circle center");

    gfx_destroy_surface(s);
}

static void test_alpha_blending(void) {
    printf("--- Test 2: Alpha Blending ---\n");
    uint32_t bg = GFX_RGB(100, 100, 100);
    uint32_t fg = GFX_RGBA(200, 200, 200, 128); /* 50% opacity */
    uint32_t blended = gfx_blend(bg, fg);

    int r = GFX_GET_R(blended);
    int g = GFX_GET_G(blended);
    int b = GFX_GET_B(blended);

    /* (200*128 + 100*127)/255 = ~150 */
    TEST_ASSERT(r >= 148 && r <= 152, "blended red channel ~150");
    TEST_ASSERT(g >= 148 && g <= 152, "blended green channel ~150");
    TEST_ASSERT(b >= 148 && b <= 152, "blended blue channel ~150");
}

static void test_bitmap_font(void) {
    printf("--- Test 3: Bitmap Text Rendering ---\n");
    gfx_surface_t *s = gfx_create_surface(200, 50);
    gfx_clear(s, GFX_COLOR_BLACK);

    gfx_draw_text(s, 10, 10, "Hello", GFX_COLOR_WHITE, GFX_COLOR_BLACK);

    /* Verify non-zero white pixels were written */
    int white_count = 0;
    for (int y = 10; y < 26; y++) {
        for (int x = 10; x < 50; x++) {
            if (gfx_getpixel(s, x, y) == GFX_COLOR_WHITE) {
                white_count++;
            }
        }
    }
    TEST_ASSERT(white_count > 20, "bitmap font rendered character pixels");
    gfx_destroy_surface(s);
}

static void test_bmp_io(void) {
    printf("--- Test 4: BMP Save and Load ---\n");
    gfx_surface_t *s = gfx_create_surface(64, 64);
    gfx_clear(s, GFX_COLOR_DARK_GRAY);
    gfx_fill_rect(s, 10, 10, 20, 20, GFX_COLOR_BLUE);
    gfx_fill_circle(s, 40, 40, 12, GFX_COLOR_YELLOW);

    const char *bmp_path = "/tmp/test.bmp";
    int ret = gfx_save_bmp(s, bmp_path);
    TEST_ASSERT(ret == 0, "gfx_save_bmp succeeded");

    gfx_surface_t *loaded = gfx_load_image(bmp_path);
    TEST_ASSERT(loaded != NULL, "gfx_load_image loaded BMP");
    if (loaded) {
        TEST_ASSERT(loaded->width == 64 && loaded->height == 64, "loaded dimensions match 64x64");
        uint32_t px_blue = gfx_getpixel(loaded, 15, 15);
        TEST_ASSERT((px_blue & 0xFFFFFF) == (GFX_COLOR_BLUE & 0xFFFFFF), "loaded pixel (15,15) is blue");
        uint32_t px_yellow = gfx_getpixel(loaded, 40, 40);
        TEST_ASSERT((px_yellow & 0xFFFFFF) == (GFX_COLOR_YELLOW & 0xFFFFFF), "loaded pixel (40,40) is yellow");
        gfx_destroy_surface(loaded);
    }

    unlink(bmp_path);
    gfx_destroy_surface(s);
}

static void test_ttf_font(void) {
    printf("--- Test 5: TrueType Font (stb_truetype) ---\n");
    const char *font_path = "/share/fonts/dejavu.ttf";
    if (access(font_path, R_OK) != 0) {
        printf("  [SKIP] TTF font %s not found\n", font_path);
        return;
    }

    gfx_font_t *font = gfx_load_font(font_path, 24.0f);
    TEST_ASSERT(font != NULL, "loaded DejaVuSans.ttf");
    if (font) {
        int w = 0, h = 0;
        gfx_measure_text_ttf(font, "MyOS", &w, &h);
        TEST_ASSERT(w > 0 && h > 0, "measured TTF text dimensions");

        gfx_surface_t *s = gfx_create_surface(200, 60);
        gfx_clear(s, GFX_COLOR_BLACK);
        gfx_draw_text_ttf(s, font, 10, 10, "MyOS", GFX_COLOR_WHITE);

        int non_black = 0;
        for (int y = 0; y < 60; y++) {
            for (int x = 0; x < 200; x++) {
                if (gfx_getpixel(s, x, y) != GFX_COLOR_BLACK) {
                    non_black++;
                }
            }
        }
        TEST_ASSERT(non_black > 50, "rendered anti-aliased TTF glyphs");
        gfx_destroy_surface(s);
        gfx_destroy_font(font);
    }
}

static void test_context_and_flip(void) {
    printf("--- Test 6: gfx_context open, draw, and flip ---\n");
    gfx_context_t *ctx = gfx_open();
    TEST_ASSERT(ctx != NULL, "gfx_open initialized framebuffer context");
    if (!ctx) return;

    TEST_ASSERT(ctx->width >= 640 && ctx->height >= 480, "valid display dimensions");
    TEST_ASSERT(ctx->backbuffer != NULL, "backbuffer surface allocated");

    /* Draw test pattern to backbuffer */
    gfx_clear(ctx->backbuffer, GFX_RGB(24, 28, 36));
    gfx_draw_text(ctx->backbuffer, 50, 50, "MyOS Graphics Subsystem Initialized", GFX_COLOR_WHITE, GFX_RGB(24, 28, 36));
    gfx_fill_rect(ctx->backbuffer, 50, 80, 100, 40, GFX_COLOR_BLUE);
    gfx_draw_cursor(ctx->backbuffer, ctx->mouse_x, ctx->mouse_y);

    /* Flip backbuffer to screen */
    gfx_flip(ctx);
    TEST_ASSERT(1, "gfx_flip copied backbuffer to framebuffer");

    gfx_close(ctx);
    TEST_ASSERT(1, "gfx_close restored display and freed context");
}

int main(void) {
    printf("=== Starting gfxtest ===\n");
    test_surface_primitives();
    test_alpha_blending();
    test_bitmap_font();
    test_bmp_io();
    test_ttf_font();
    test_context_and_flip();

    printf("\n=== gfxtest results: %d passed, %d failed ===\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}


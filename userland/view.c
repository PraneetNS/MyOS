#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "gfx.h"

static gfx_context_t *g_ctx = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_ctx) {
        gfx_close(g_ctx);
        g_ctx = NULL;
    }
    _exit(0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image-path> [--still]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int still_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--still") == 0) {
            still_mode = 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);

    /* Load image */
    gfx_surface_t *img = gfx_load_image(path);
    if (!img) {
        fprintf(stderr, "view: failed to load image '%s'\n", path);
        return 1;
    }

    /* Open graphics display */
    gfx_context_t *ctx = gfx_open();
    if (!ctx) {
        fprintf(stderr, "view: failed to open graphics display\n");
        gfx_destroy_surface(img);
        return 1;
    }
    g_ctx = ctx;

    /* Dark sleek background */
    gfx_clear(ctx->backbuffer, GFX_RGB(18, 22, 28));

    /* Top title bar */
    gfx_fill_rect(ctx->backbuffer, 0, 0, ctx->width, 32, GFX_RGB(28, 34, 44));
    gfx_draw_line(ctx->backbuffer, 0, 32, ctx->width, 32, GFX_RGB(45, 55, 72));

    char title[128];
    snprintf(title, sizeof(title), "MyOS Image Viewer - %s (%dx%d)", path, img->width, img->height);
    gfx_draw_text(ctx->backbuffer, 12, 8, title, GFX_RGB(240, 240, 245), GFX_RGB(28, 34, 44));

    /* Bottom status bar */
    int bar_y = ctx->height - 24;
    gfx_fill_rect(ctx->backbuffer, 0, bar_y, ctx->width, 24, GFX_RGB(24, 28, 36));
    gfx_draw_line(ctx->backbuffer, 0, bar_y, ctx->width, bar_y, GFX_RGB(45, 55, 72));
    gfx_draw_text(ctx->backbuffer, 12, bar_y + 4, "Press ESC, Q, or click to exit", GFX_RGB(160, 170, 185), GFX_RGB(24, 28, 36));

    /* Center image in remaining viewport */
    int avail_w = ctx->width;
    int avail_h = ctx->height - 56;
    int ix = (avail_w - img->width) / 2;
    int iy = 32 + (avail_h - img->height) / 2;

    if (ix < 4) ix = 4;
    if (iy < 36) iy = 36;

    /* Border around image */
    gfx_draw_rect(ctx->backbuffer, ix - 1, iy - 1, img->width + 2, img->height + 2, GFX_RGB(60, 75, 95));

    /* Blit full image */
    gfx_blit(ctx->backbuffer, ix, iy, img, 0, 0, img->width, img->height);

    /* Flip to screen */
    gfx_flip(ctx);

    if (still_mode) {
        usleep(300000); /* brief display in still mode */
    } else {
        /* Interactive event loop */
        int running = 1;
        while (running) {
            gfx_event_t ev;
            while (gfx_poll_event(ctx, &ev)) {
                if (ev.type == GFX_EVENT_KEY_DOWN) {
                    if (ev.key.keycode == 1 || /* ESC */
                        ev.key.keycode == 16) { /* Q */
                        running = 0;
                        break;
                    }
                } else if (ev.type == GFX_EVENT_MOUSE_DOWN) {
                    running = 0;
                    break;
                }
            }
            usleep(20000);
        }
    }

    int w = img->width, h = img->height;
    gfx_close(ctx);
    g_ctx = NULL;
    gfx_destroy_surface(img);
    printf("[view] Displayed %dx%d image %s\n", w, h, path);
    return 0;
}

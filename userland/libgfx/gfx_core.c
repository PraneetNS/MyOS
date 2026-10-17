#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/kd.h>

gfx_surface_t *gfx_create_surface(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    gfx_surface_t *s = (gfx_surface_t *) malloc(sizeof(gfx_surface_t));
    if (!s) return NULL;

    s->width = width;
    s->height = height;
    s->pitch = width * sizeof(uint32_t);
    s->bpp = sizeof(uint32_t);
    s->pixels = (uint32_t *) calloc(width * height, sizeof(uint32_t));
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->owns_pixels = 1;
    gfx_reset_clip(s);
    return s;
}

void gfx_destroy_surface(gfx_surface_t *s) {
    if (!s) return;
    if (s->owns_pixels && s->pixels) {
        free(s->pixels);
    }
    free(s);
}

void gfx_set_clip(gfx_surface_t *s, int x0, int y0, int x1, int y1) {
    if (!s) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip.x0 = x0;
    s->clip.y0 = y0;
    s->clip.x1 = x1;
    s->clip.y1 = y1;
}

void gfx_reset_clip(gfx_surface_t *s) {
    if (!s) return;
    s->clip.x0 = 0;
    s->clip.y0 = 0;
    s->clip.x1 = s->width;
    s->clip.y1 = s->height;
}

gfx_context_t *gfx_open(void) {
    int fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return NULL;

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fb_fd);
        return NULL;
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        close(fb_fd);
        return NULL;
    }

    size_t smem_len = finfo.smem_len;
    if (smem_len == 0) {
        smem_len = vinfo.yres * finfo.line_length;
    }

    void *fb_map = mmap(NULL, smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED || !fb_map) {
        close(fb_fd);
        return NULL;
    }

    gfx_context_t *ctx = (gfx_context_t *) calloc(1, sizeof(gfx_context_t));
    if (!ctx) {
        munmap(fb_map, smem_len);
        close(fb_fd);
        return NULL;
    }

    ctx->fb_fd = fb_fd;
    ctx->width = (int) vinfo.xres;
    ctx->height = (int) vinfo.yres;
    ctx->pitch = (int) finfo.line_length;
    ctx->bpp = (int) (vinfo.bits_per_pixel / 8);
    ctx->fb_mmap = (uint32_t *) fb_map;
    ctx->fb_size = smem_len;

    ctx->backbuffer = gfx_create_surface(ctx->width, ctx->height);
    if (!ctx->backbuffer) {
        munmap(fb_map, smem_len);
        close(fb_fd);
        free(ctx);
        return NULL;
    }
    ctx->backbuffer->pitch = ctx->pitch;

    /* Switch console to graphics mode & raw keyboard */
    int tty_fd = open("/dev/console", O_RDWR);
    if (tty_fd >= 0) {
        ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
        ioctl(tty_fd, KDSKBMODE, K_OFF);
    }
    ctx->tty_fd = tty_fd;

    /* Open input devices non-blocking */
    ctx->mouse_fd = open("/dev/input/mouse0", O_RDONLY | O_NONBLOCK);
    if (ctx->mouse_fd < 0) {
        /* Fallback to /dev/input/mice */
        ctx->mouse_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    }
    ctx->kbd_fd = open("/dev/input/kbd0", O_RDONLY | O_NONBLOCK);

    ctx->mouse_x = ctx->width / 2;
    ctx->mouse_y = ctx->height / 2;
    ctx->mouse_buttons = 0;
    ctx->cursor_visible = 1;

    return ctx;
}

void gfx_close(gfx_context_t *ctx) {
    if (!ctx) return;

    /* Restore console text mode and keyboard */
    if (ctx->tty_fd >= 0) {
        ioctl(ctx->tty_fd, KDSKBMODE, K_XLATE);
        ioctl(ctx->tty_fd, KDSETMODE, KD_TEXT);
        close(ctx->tty_fd);
        ctx->tty_fd = -1;
    }

    if (ctx->mouse_fd >= 0) close(ctx->mouse_fd);
    if (ctx->kbd_fd >= 0) close(ctx->kbd_fd);

    if (ctx->backbuffer) {
        gfx_destroy_surface(ctx->backbuffer);
        ctx->backbuffer = NULL;
    }

    if (ctx->fb_mmap && ctx->fb_size > 0) {
        munmap(ctx->fb_mmap, ctx->fb_size);
    }
    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
    }

    free(ctx);
}

void gfx_flip(gfx_context_t *ctx) {
    if (!ctx || !ctx->fb_mmap || !ctx->backbuffer) return;
    memcpy(ctx->fb_mmap, ctx->backbuffer->pixels, (size_t)(ctx->height * ctx->pitch));
}

void gfx_flip_rect(gfx_context_t *ctx, int x, int y, int w, int h) {
    if (!ctx || !ctx->fb_mmap || !ctx->backbuffer) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ctx->width) w = ctx->width - x;
    if (y + h > ctx->height) h = ctx->height - y;
    if (w <= 0 || h <= 0) return;

    int bytes_per_line = w * sizeof(uint32_t);
    int pitch_words = ctx->pitch / sizeof(uint32_t);

    for (int r = 0; r < h; r++) {
        uint32_t *dst = ctx->fb_mmap + (y + r) * pitch_words + x;
        const uint32_t *src = ctx->backbuffer->pixels + (y + r) * pitch_words + x;
        memcpy(dst, src, bytes_per_line);
    }
}

#include "gfx.h"
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* Cursor bitmap: 11x16 classic pointer arrow */
static const char *cursor_shape[16] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X.....XXXX ",
    "X..X..X    ",
    "X.X X..X   ",
    "XX   X..X  ",
    "X     X..X ",
    "       XX  ",
    "           "
};

void gfx_draw_cursor(gfx_surface_t *s, int x, int y) {
    if (!s) return;
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < s->clip.y0 || py >= s->clip.y1) continue;

        for (int col = 0; col < 11; col++) {
            int px = x + col;
            if (px < s->clip.x0 || px >= s->clip.x1) continue;

            char ch = cursor_shape[row][col];
            if (ch == 'X') {
                gfx_putpixel(s, px, py, GFX_COLOR_BLACK);
            } else if (ch == '.') {
                gfx_putpixel(s, px, py, GFX_COLOR_WHITE);
            }
        }
    }
}

int gfx_poll_event(gfx_context_t *ctx, gfx_event_t *event) {
    if (!ctx || !event) return 0;
    event->type = GFX_EVENT_NONE;

    /* 1. Poll Keyboard */
    if (ctx->kbd_fd >= 0) {
        struct input_event kev;
        int n = read(ctx->kbd_fd, &kev, sizeof(kev));
        if (n == (int) sizeof(kev)) {
            if (kev.type == EV_KEY) {
                event->type = (kev.value != 0) ? GFX_EVENT_KEY_DOWN : GFX_EVENT_KEY_UP;
                event->key.keycode = kev.code;
                uint32_t mods = 0;
                ioctl(ctx->kbd_fd, KBD_IOC_GETMODS, &mods);
                event->key.modifiers = (int) mods;
                return 1;
            }
        }
    }

    /* 2. Poll Mouse */
    if (ctx->mouse_fd >= 0) {
        struct input_event mev;
        int n = read(ctx->mouse_fd, &mev, sizeof(mev));
        if (n == (int) sizeof(mev)) {
            if (mev.type == EV_REL) {
                if (mev.code == REL_X) {
                    ctx->mouse_x += mev.value;
                    if (ctx->mouse_x < 0) ctx->mouse_x = 0;
                    if (ctx->mouse_x >= ctx->width) ctx->mouse_x = ctx->width - 1;

                    event->type = GFX_EVENT_MOUSE_MOVE;
                    event->mouse.x = ctx->mouse_x;
                    event->mouse.y = ctx->mouse_y;
                    event->mouse.dx = mev.value;
                    event->mouse.dy = 0;
                    event->mouse.buttons = ctx->mouse_buttons;
                    return 1;
                } else if (mev.code == REL_Y) {
                    ctx->mouse_y += mev.value;
                    if (ctx->mouse_y < 0) ctx->mouse_y = 0;
                    if (ctx->mouse_y >= ctx->height) ctx->mouse_y = ctx->height - 1;

                    event->type = GFX_EVENT_MOUSE_MOVE;
                    event->mouse.x = ctx->mouse_x;
                    event->mouse.y = ctx->mouse_y;
                    event->mouse.dx = 0;
                    event->mouse.dy = mev.value;
                    event->mouse.buttons = ctx->mouse_buttons;
                    return 1;
                } else if (mev.code == REL_WHEEL) {
                    event->type = GFX_EVENT_MOUSE_WHEEL;
                    event->mouse.x = ctx->mouse_x;
                    event->mouse.y = ctx->mouse_y;
                    event->mouse.wheel = mev.value;
                    event->mouse.buttons = ctx->mouse_buttons;
                    return 1;
                }
            } else if (mev.type == EV_KEY) {
                int btn = 0;
                if (mev.code == BTN_LEFT) btn = 1;
                else if (mev.code == BTN_RIGHT) btn = 2;
                else if (mev.code == BTN_MIDDLE) btn = 3;

                if (mev.value) {
                    ctx->mouse_buttons |= (1 << (btn - 1));
                    event->type = GFX_EVENT_MOUSE_DOWN;
                } else {
                    ctx->mouse_buttons &= ~(1 << (btn - 1));
                    event->type = GFX_EVENT_MOUSE_UP;
                }
                event->mouse.x = ctx->mouse_x;
                event->mouse.y = ctx->mouse_y;
                event->mouse.button = btn;
                event->mouse.buttons = ctx->mouse_buttons;
                return 1;
            }
        }
    }

    return 0;
}

int gfx_wait_event(gfx_context_t *ctx, gfx_event_t *event, int timeout_ms) {
    if (!ctx || !event) return 0;

    /* First check if an event is already immediately readable */
    if (gfx_poll_event(ctx, event)) {
        return 1;
    }

    struct pollfd pfds[2];
    int pcount = 0;

    if (ctx->kbd_fd >= 0) {
        pfds[pcount].fd = ctx->kbd_fd;
        pfds[pcount].events = POLLIN;
        pfds[pcount].revents = 0;
        pcount++;
    }
    if (ctx->mouse_fd >= 0) {
        pfds[pcount].fd = ctx->mouse_fd;
        pfds[pcount].events = POLLIN;
        pfds[pcount].revents = 0;
        pcount++;
    }

    if (pcount == 0) return 0;

    int ret = poll(pfds, pcount, timeout_ms);
    if (ret > 0) {
        return gfx_poll_event(ctx, event);
    }

    return 0;
}

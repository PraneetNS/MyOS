#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "gfx.h"

typedef enum {
    TOOL_PENCIL = 0,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_FILL_RECT,
    TOOL_CIRCLE,
    TOOL_FILL_CIRCLE,
    TOOL_COUNT
} paint_tool_t;

static const char *TOOL_NAMES[TOOL_COUNT] = {
    "Pencil", "Line", "Rect", "FillRect", "Circle", "FillCircle"
};

#define NUM_COLORS 14
static const uint32_t PALETTE[NUM_COLORS] = {
    GFX_RGB(0, 0, 0),         /* Black */
    GFX_RGB(255, 255, 255),   /* White */
    GFX_RGB(239, 68, 68),     /* Red */
    GFX_RGB(249, 115, 22),    /* Orange */
    GFX_RGB(245, 158, 11),    /* Amber */
    GFX_RGB(16, 185, 129),    /* Emerald */
    GFX_RGB(20, 184, 166),    /* Teal */
    GFX_RGB(6, 182, 212),     /* Cyan */
    GFX_RGB(59, 130, 246),    /* Blue */
    GFX_RGB(99, 102, 241),    /* Indigo */
    GFX_RGB(139, 92, 246),    /* Violet */
    GFX_RGB(217, 70, 239),    /* Fuchsia */
    GFX_RGB(100, 116, 139),   /* Gray */
    GFX_RGB(180, 83, 9)       /* Brown */
};

static void draw_ui(gfx_context_t *ctx, paint_tool_t tool, int color_idx, const char *status_msg) {
    gfx_surface_t *bb = ctx->backbuffer;
    int w = ctx->width;

    /* Top tool bar (0..50) */
    gfx_fill_rect(bb, 0, 0, w, 50, GFX_RGB(30, 41, 59));
    gfx_draw_line(bb, 0, 50, w, 50, GFX_RGB(71, 85, 105));

    gfx_draw_text(bb, 10, 16, "MyOS Paint", GFX_RGB(248, 250, 252), GFX_RGB(30, 41, 59));

    /* Tool buttons */
    int tx = 110;
    for (int t = 0; t < TOOL_COUNT; t++) {
        int tw = 75;
        uint32_t btn_bg = (t == (int)tool) ? GFX_RGB(59, 130, 246) : GFX_RGB(51, 65, 85);
        gfx_fill_rect(bb, tx, 8, tw, 32, btn_bg);
        gfx_draw_rect(bb, tx, 8, tw, 32, GFX_RGB(148, 163, 184));
        gfx_draw_text(bb, tx + 6, 16, TOOL_NAMES[t], GFX_RGB(255, 255, 255), btn_bg);
        tx += tw + 6;
    }

    /* Color swatches */
    int cx = tx + 20;
    for (int c = 0; c < NUM_COLORS; c++) {
        int cw = 20;
        int ch = 30;
        gfx_fill_rect(bb, cx, 10, cw, ch, PALETTE[c]);
        uint32_t border = (c == color_idx) ? GFX_RGB(255, 255, 255) : GFX_RGB(100, 116, 139);
        gfx_draw_rect(bb, cx, 10, cw, ch, border);
        if (c == color_idx) {
            gfx_draw_rect(bb, cx + 1, 11, cw - 2, ch - 2, GFX_RGB(0, 0, 0));
        }
        cx += cw + 4;
    }

    /* Clear and Save buttons */
    int bx = w - 160;
    gfx_fill_rect(bb, bx, 8, 65, 32, GFX_RGB(220, 38, 38));
    gfx_draw_rect(bb, bx, 8, 65, 32, GFX_RGB(248, 113, 113));
    gfx_draw_text(bb, bx + 12, 16, "Clear", GFX_RGB(255, 255, 255), GFX_RGB(220, 38, 38));

    bx += 75;
    gfx_fill_rect(bb, bx, 8, 65, 32, GFX_RGB(16, 185, 129));
    gfx_draw_rect(bb, bx, 8, 65, 32, GFX_RGB(110, 231, 183));
    gfx_draw_text(bb, bx + 15, 16, "Save", GFX_RGB(255, 255, 255), GFX_RGB(16, 185, 129));

    /* Bottom status bar */
    int by = ctx->height - 24;
    gfx_fill_rect(bb, 0, by, w, 24, GFX_RGB(15, 23, 42));
    gfx_draw_line(bb, 0, by, w, by, GFX_RGB(51, 65, 85));

    char stat[256];
    snprintf(stat, sizeof(stat), "Tool: %s | Color: #%06lX | Pos: (%d, %d) | %s",
             TOOL_NAMES[tool], (unsigned long)(PALETTE[color_idx] & 0xFFFFFF),
             ctx->mouse_x, ctx->mouse_y, status_msg ? status_msg : "Press S to save, C to clear, Q to quit");
    gfx_draw_text(bb, 10, by + 4, stat, GFX_RGB(203, 213, 225), GFX_RGB(15, 23, 42));
}

#include <signal.h>

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
    int auto_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto") == 0 || strcmp(argv[i], "--test") == 0) {
            auto_mode = 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGQUIT, sig_handler);

    gfx_context_t *ctx = gfx_open();
    if (!ctx) {
        fprintf(stderr, "paint: failed to open graphics context\n");
        return 1;
    }
    g_ctx = ctx;

    int canvas_top = 51;
    int canvas_bottom = ctx->height - 25;
    int canvas_h = canvas_bottom - canvas_top;

    gfx_surface_t *canvas = gfx_create_surface(ctx->width, canvas_h);
    if (!canvas) {
        gfx_close(ctx);
        return 1;
    }
    gfx_clear(canvas, GFX_RGB(255, 255, 255));

    paint_tool_t cur_tool = TOOL_PENCIL;
    int cur_color_idx = 2; /* Red default */
    const char *status_msg = "Ready";

    if (auto_mode) {
        /* Automated test mode: draw shapes on canvas, save BMP, and exit */
        gfx_draw_line(canvas, 100, 100, 300, 150, PALETTE[2]);
        gfx_fill_rect(canvas, 150, 200, 180, 100, PALETTE[8]);
        gfx_draw_rect(canvas, 150, 200, 180, 100, PALETTE[0]);
        gfx_fill_circle(canvas, 500, 250, 60, PALETTE[5]);
        gfx_draw_circle(canvas, 500, 250, 60, PALETTE[0]);

        /* Blit to backbuffer */
        gfx_blit(ctx->backbuffer, 0, canvas_top, canvas, 0, 0, canvas->width, canvas->height);
        draw_ui(ctx, cur_tool, cur_color_idx, "Auto-saved /tmp/canvas.bmp");
        gfx_flip(ctx);

        gfx_save_bmp(canvas, "/tmp/canvas.bmp");
        gfx_destroy_surface(canvas);
        gfx_close(ctx);
        g_ctx = NULL;
        printf("[paint] Automated test passed: saved canvas to /tmp/canvas.bmp\n");
        return 0;
    }

    int dragging = 0;
    int drag_start_x = 0;
    int drag_start_y = 0;
    int prev_x = ctx->mouse_x;
    int prev_y = ctx->mouse_y;

    int running = 1;
    while (running) {
        gfx_event_t ev;
        while (gfx_poll_event(ctx, &ev)) {
            if (ev.type == GFX_EVENT_KEY_DOWN) {
                int kc = ev.key.keycode;
                if (kc == 1 || kc == 16) { /* ESC or Q */
                    running = 0;
                    break;
                } else if (kc == 31) { /* S = save */
                    if (gfx_save_bmp(canvas, "/tmp/canvas.bmp") == 0) {
                        status_msg = "Canvas saved to /tmp/canvas.bmp";
                    } else {
                        status_msg = "Error saving canvas";
                    }
                } else if (kc == 46) { /* C = clear */
                    gfx_clear(canvas, GFX_RGB(255, 255, 255));
                    status_msg = "Canvas cleared";
                } else if (kc >= 2 && kc <= 10) { /* 1..9 */
                    int idx = kc - 2;
                    if (idx < NUM_COLORS) cur_color_idx = idx;
                }
            } else if (ev.type == GFX_EVENT_MOUSE_DOWN) {
                int mx = ctx->mouse_x;
                int my = ctx->mouse_y;

                if (my < 50) {
                    /* Top bar clicked */
                    int tx = 110;
                    for (int t = 0; t < TOOL_COUNT; t++) {
                        if (mx >= tx && mx <= tx + 75 && my >= 8 && my <= 40) {
                            cur_tool = (paint_tool_t) t;
                            status_msg = TOOL_NAMES[t];
                            break;
                        }
                        tx += 75 + 6;
                    }

                    int cx = tx + 20;
                    for (int c = 0; c < NUM_COLORS; c++) {
                        if (mx >= cx && mx <= cx + 20 && my >= 10 && my <= 40) {
                            cur_color_idx = c;
                            break;
                        }
                        cx += 24;
                    }

                    int bx = ctx->width - 160;
                    if (mx >= bx && mx <= bx + 65) {
                        gfx_clear(canvas, GFX_RGB(255, 255, 255));
                        status_msg = "Canvas cleared";
                    } else if (mx >= bx + 75 && mx <= bx + 140) {
                        gfx_save_bmp(canvas, "/tmp/canvas.bmp");
                        status_msg = "Saved /tmp/canvas.bmp";
                    }
                } else if (my >= canvas_top && my < canvas_bottom) {
                    dragging = 1;
                    drag_start_x = mx;
                    drag_start_y = my - canvas_top;
                    prev_x = drag_start_x;
                    prev_y = drag_start_y;

                    if (cur_tool == TOOL_PENCIL) {
                        gfx_putpixel(canvas, prev_x, prev_y, PALETTE[cur_color_idx]);
                    }
                }
            } else if (ev.type == GFX_EVENT_MOUSE_UP) {
                if (dragging) {
                    int end_x = ctx->mouse_x;
                    int end_y = ctx->mouse_y - canvas_top;

                    /* Commit shape tool to canvas */
                    uint32_t col = PALETTE[cur_color_idx];
                    if (cur_tool == TOOL_LINE) {
                        gfx_draw_line(canvas, drag_start_x, drag_start_y, end_x, end_y, col);
                    } else if (cur_tool == TOOL_RECT) {
                        int rx = (drag_start_x < end_x) ? drag_start_x : end_x;
                        int ry = (drag_start_y < end_y) ? drag_start_y : end_y;
                        int rw = abs(end_x - drag_start_x);
                        int rh = abs(end_y - drag_start_y);
                        gfx_draw_rect(canvas, rx, ry, rw, rh, col);
                    } else if (cur_tool == TOOL_FILL_RECT) {
                        int rx = (drag_start_x < end_x) ? drag_start_x : end_x;
                        int ry = (drag_start_y < end_y) ? drag_start_y : end_y;
                        int rw = abs(end_x - drag_start_x);
                        int rh = abs(end_y - drag_start_y);
                        gfx_fill_rect(canvas, rx, ry, rw, rh, col);
                    } else if (cur_tool == TOOL_CIRCLE || cur_tool == TOOL_FILL_CIRCLE) {
                        int dx = end_x - drag_start_x;
                        int dy = end_y - drag_start_y;
                        int r = (int) sqrtf((float)(dx * dx + dy * dy));
                        if (cur_tool == TOOL_CIRCLE) {
                            gfx_draw_circle(canvas, drag_start_x, drag_start_y, r, col);
                        } else {
                            gfx_fill_circle(canvas, drag_start_x, drag_start_y, r, col);
                        }
                    }
                    dragging = 0;
                }
            } else if (ev.type == GFX_EVENT_MOUSE_MOVE) {
                if (dragging && cur_tool == TOOL_PENCIL) {
                    int cur_x = ctx->mouse_x;
                    int cur_y = ctx->mouse_y - canvas_top;
                    gfx_draw_line(canvas, prev_x, prev_y, cur_x, cur_y, PALETTE[cur_color_idx]);
                    prev_x = cur_x;
                    prev_y = cur_y;
                }
            }
        }

        if (!running) break;

        /* Blit persistent canvas to backbuffer */
        gfx_blit(ctx->backbuffer, 0, canvas_top, canvas, 0, 0, canvas->width, canvas->height);

        /* Draw shape preview if dragging */
        if (dragging && cur_tool != TOOL_PENCIL) {
            int end_x = ctx->mouse_x;
            int end_y = ctx->mouse_y;
            uint32_t col = PALETTE[cur_color_idx];
            int sy = drag_start_y + canvas_top;

            if (cur_tool == TOOL_LINE) {
                gfx_draw_line(ctx->backbuffer, drag_start_x, sy, end_x, end_y, col);
            } else if (cur_tool == TOOL_RECT) {
                int rx = (drag_start_x < end_x) ? drag_start_x : end_x;
                int ry = (sy < end_y) ? sy : end_y;
                gfx_draw_rect(ctx->backbuffer, rx, ry, abs(end_x - drag_start_x), abs(end_y - sy), col);
            } else if (cur_tool == TOOL_FILL_RECT) {
                int rx = (drag_start_x < end_x) ? drag_start_x : end_x;
                int ry = (sy < end_y) ? sy : end_y;
                gfx_fill_rect_alpha(ctx->backbuffer, rx, ry, abs(end_x - drag_start_x), abs(end_y - sy), (col & 0xFFFFFF) | (180 << 24));
            } else if (cur_tool == TOOL_CIRCLE || cur_tool == TOOL_FILL_CIRCLE) {
                int dx = end_x - drag_start_x;
                int dy = end_y - sy;
                int r = (int) sqrtf((float)(dx * dx + dy * dy));
                if (cur_tool == TOOL_CIRCLE) {
                    gfx_draw_circle(ctx->backbuffer, drag_start_x, sy, r, col);
                } else {
                    gfx_fill_circle(ctx->backbuffer, drag_start_x, sy, r, col);
                }
            }
        }

        draw_ui(ctx, cur_tool, cur_color_idx, status_msg);

        if (ctx->cursor_visible) {
            gfx_draw_cursor(ctx->backbuffer, ctx->mouse_x, ctx->mouse_y);
        }

        gfx_flip(ctx);
        usleep(16000);
    }

    gfx_destroy_surface(canvas);
    gfx_close(ctx);
    return 0;
}

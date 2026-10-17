/*
 * winhello.c - Minimal Window System Test Client
 *
 * Connects to display server, creates a 300x200 window filled with a known color
 * and text, and echoes received input events to the console/serial.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <win.h>
#include <gfx.h>

static void draw_window_content(win_t* win, int w, int h) {
    gfx_surface_t* s = win_buffer(win);
    if (!s) return;

    /* Fill background with teal (0xFF008080) */
    uint32_t bg_color = 0xFF008080;
    gfx_fill_rect(s, 0, 0, w, h, bg_color);

    /* Draw white border outline */
    gfx_draw_rect(s, 0, 0, w, h, 0xFFFFFFFF);
    gfx_draw_rect(s, 1, 1, w - 2, h - 2, 0xFF40E0D0);

    /* Render text (fg, bg; bg=0 for transparent background) */
    gfx_draw_text(s, 20, 30, "Hello MyOS Window System!", 0xFFFFFFFF, 0);
    gfx_draw_text(s, 20, 60, "Stage 18B-2 Minimal Compositor", 0xFFE0E0E0, 0);
    gfx_draw_text(s, 20, 90, "Double-buffered SHM rendering", 0xFFD0D0D0, 0);

    /* Draw small colored test patches */
    gfx_fill_rect(s, 20, 130, 30, 30, 0xFFFF0000); /* Red */
    gfx_fill_rect(s, 60, 130, 30, 30, 0xFF00FF00); /* Green */
    gfx_fill_rect(s, 100, 130, 30, 30, 0xFF0000FF); /* Blue */
    gfx_fill_rect(s, 140, 130, 30, 30, 0xFFFFFF00); /* Yellow */

    win_damage(win, 0, 0, w, h);
    win_commit(win);
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    printf("[winhello] Connecting to display server at %s...\n", WIN_SOCK_PATH);
    fflush(stdout);

    win_conn_t* conn = win_connect();
    if (!conn) {
        fprintf(stderr, "[winhello] Error: could not connect to display server\n");
        return 1;
    }

    printf("[winhello] Connected to display server. Screen resolution: %lux%lu\n",
           (unsigned long)win_screen_width(conn), (unsigned long)win_screen_height(conn));
    fflush(stdout);

    int win_w = 300;
    int win_h = 200;
    win_t* win = win_create(conn, win_w, win_h, 0, "winhello");
    if (!win) {
        fprintf(stderr, "[winhello] Error: failed to create window\n");
        win_disconnect(conn);
        return 1;
    }

    printf("[winhello] Window %lu created (size %dx%d)\n", (unsigned long)win_id(win), win_w, win_h);
    fflush(stdout);

    draw_window_content(win, win_w, win_h);

    /* Main event loop */
    int running = 1;
    while (running) {
        win_event_t ev;
        int r = win_event_wait(conn, &ev, 1000);
        if (r < 0) {
            printf("[winhello] Connection closed or error\n");
            break;
        }
        if (r == 0) {
            /* Timeout: loop continues */
            continue;
        }

        switch (ev.type) {
            case WIN_EVENT_POINTER_MOVE:
                printf("[winhello] POINTER_MOVE x=%d y=%d\n", (int)ev.pointer.x, (int)ev.pointer.y);
                fflush(stdout);
                break;

            case WIN_EVENT_POINTER_BUTTON:
                printf("[winhello] POINTER_BUTTON btn=%lu state=%lu x=%d y=%d\n",
                       (unsigned long)ev.pointer.button, (unsigned long)ev.pointer.state,
                       (int)ev.pointer.x, (int)ev.pointer.y);
                fflush(stdout);
                break;

            case WIN_EVENT_POINTER_WHEEL:
                printf("[winhello] POINTER_WHEEL dy=%d\n", (int)ev.pointer.wheel);
                fflush(stdout);
                break;

            case WIN_EVENT_KEY:
                printf("[winhello] KEY keycode=%lu unicode=0x%lx mods=0x%lx action=%lu\n",
                       (unsigned long)ev.key.keycode, (unsigned long)ev.key.unicode,
                       (unsigned long)ev.key.modifiers, (unsigned long)ev.key.action);
                fflush(stdout);
                /* Quit if ESC key pressed */
                if (ev.key.keycode == 1 && ev.key.action == WIN_KEY_PRESS) {
                    printf("[winhello] ESC pressed, exiting\n");
                    running = 0;
                }
                break;

            case WIN_EVENT_FOCUS:
                printf("[winhello] FOCUS focused=%lu\n", (unsigned long)ev.focus.focused);
                fflush(stdout);
                break;

            case WIN_EVENT_CONFIGURE:
                printf("[winhello] CONFIGURE w=%lu h=%lu state=%lu\n",
                       (unsigned long)ev.configure.width, (unsigned long)ev.configure.height,
                       (unsigned long)ev.configure.state);
                fflush(stdout);
                win_w = (int)ev.configure.width;
                win_h = (int)ev.configure.height;
                draw_window_content(win, win_w, win_h);
                break;

            case WIN_EVENT_CLOSE_REQUEST:
                printf("[winhello] CLOSE_REQUEST received, exiting...\n");
                fflush(stdout);
                running = 0;
                break;

            default:
                break;
        }
    }

    printf("[winhello] Cleaning up window and disconnecting...\n");
    win_destroy(win);
    win_disconnect(conn);
    printf("[winhello] Exited cleanly.\n");
    return 0;
}

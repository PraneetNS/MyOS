/*
 * ui_core.c - MyOS Widget Toolkit Core Architecture & Event Dispatcher
 *
 * Stage 18B-2 Step 3a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui_internal.h"

static ui_theme_t s_default_theme = {
    .bg_color        = 0xFFF1F5F9, /* Light slate background */
    .panel_bg        = 0xFFFFFFFF, /* White */
    .text_color      = 0xFF0F172A, /* Deep slate text */
    .text_secondary  = 0xFF64748B, /* Muted slate */
    .primary         = 0xFF2563EB, /* Royal Blue */
    .primary_hover   = 0xFF1D4ED8, /* Darker Blue */
    .primary_active  = 0xFF1E40AF, /* Deep Navy */
    .border_color    = 0xFFCBD5E1, /* Light gray border */
    .focus_ring      = 0xFF38BDF8, /* Sky Blue outline */
    .selection_bg    = 0xFF93C5FD  /* Soft blue selection */
};

ui_theme_t* ui_theme_get_default(void) {
    return &s_default_theme;
}

ui_app_t* ui_app_create(void) {
    win_conn_t* conn = win_connect();
    if (!conn) return NULL;

    ui_app_t* app = (ui_app_t*) calloc(1, sizeof(ui_app_t));
    if (!app) {
        win_disconnect(conn);
        return NULL;
    }

    app->conn = conn;
    app->theme = s_default_theme;
    app->cap_windows = 8;
    app->windows = (ui_window_t**) calloc(app->cap_windows, sizeof(ui_window_t*));
    app->running = 1;
    return app;
}

void ui_app_destroy(ui_app_t* app) {
    if (!app) return;
    for (int i = 0; i < app->num_windows; i++) {
        if (app->windows[i]) {
            ui_window_destroy(app->windows[i]);
            app->windows[i] = NULL;
        }
    }
    if (app->windows) free(app->windows);
    if (app->conn) win_disconnect(app->conn);
    free(app);
}

void ui_app_quit(ui_app_t* app) {
    if (app) app->running = 0;
}

win_t* ui_window_get_raw(ui_window_t* win) {
    return win ? win->raw_win : NULL;
}

ui_widget_t* ui_window_get_root(ui_window_t* win) {
    return win ? win->root : NULL;
}

void ui_window_invalidate(ui_window_t* win) {
    if (win) win->dirty = 1;
}

void ui_widget_invalidate(ui_widget_t* w) {
    if (w && w->win) w->win->dirty = 1;
}

static void free_widget_tree(ui_widget_t* w) {
    if (!w) return;
    for (int i = 0; i < w->num_children; i++) {
        free_widget_tree(w->children[i]);
    }
    if (w->children) free(w->children);

    if (w->type == UI_WIDGET_TEXTAREA && w->textarea.buffer) {
        free(w->textarea.buffer);
    } else if (w->type == UI_WIDGET_LISTBOX && w->listbox.items) {
        for (int i = 0; i < w->listbox.num_items; i++) {
            if (w->listbox.items[i]) free(w->listbox.items[i]);
        }
        free(w->listbox.items);
    }
    free(w);
}

ui_window_t* ui_window_create(ui_app_t* app, int width, int height, const char* title) {
    if (!app) return NULL;

    win_t* raw = win_create(app->conn, width, height, 0, title);
    if (!raw) return NULL;

    ui_window_t* win = (ui_window_t*) calloc(1, sizeof(ui_window_t));
    if (!win) {
        win_destroy(raw);
        return NULL;
    }

    win->app = app;
    win->raw_win = raw;
    win->width = width;
    win->height = height;
    strncpy(win->title, title ? title : "MyOS Window", sizeof(win->title) - 1);
    win->dirty = 1;
    win->running = 1;
    win->next_a11y_id = 1;

    /* Create root container widget */
    win->root = ui_container_create(win, 0, 0, width, height);

    /* Hook accessibility callbacks */
    win_set_a11y_handlers(raw, ui_a11y_on_get_tree, ui_a11y_on_action, win);

    if (app->num_windows >= app->cap_windows) {
        app->cap_windows *= 2;
        app->windows = (ui_window_t**) realloc(app->windows, app->cap_windows * sizeof(ui_window_t*));
    }
    app->windows[app->num_windows++] = win;

    return win;
}

void ui_window_destroy(ui_window_t* win) {
    if (!win) return;
    if (win->root) {
        free_widget_tree(win->root);
        win->root = NULL;
    }
    if (win->raw_win) {
        win_destroy(win->raw_win);
        win->raw_win = NULL;
    }
    free(win);
}

void ui_widget_add_child(ui_widget_t* parent, ui_widget_t* child) {
    if (!parent || !child) return;
    if (parent->num_children >= parent->cap_children) {
        parent->cap_children = (parent->cap_children == 0) ? 4 : parent->cap_children * 2;
        parent->children = (ui_widget_t**) realloc(parent->children, parent->cap_children * sizeof(ui_widget_t*));
    }
    parent->children[parent->num_children++] = child;
    child->parent = parent;
    child->win = parent->win;
    if (child->a11y_id == 0 && parent->win) {
        child->a11y_id = parent->win->next_a11y_id++;
    }
}

void ui_widget_set_focus(ui_widget_t* w) {
    if (!w || !w->win) return;
    if (w->win->focused == w) return;
    if (w->win->focused) {
        w->win->focused->flags &= ~UI_STATE_FOCUSED;
    }
    w->win->focused = w;
    w->flags |= UI_STATE_FOCUSED;
    w->win->dirty = 1;
}

static void get_absolute_bounds(ui_widget_t* w, int* ax, int* ay, int* aw, int* ah) {
    int x = w->x;
    int y = w->y;
    ui_widget_t* p = w->parent;
    while (p) {
        x += p->x;
        y += p->y;
        p = p->parent;
    }
    *ax = x;
    *ay = y;
    *aw = w->w;
    *ah = w->h;
}

static ui_widget_t* hit_test_widget(ui_widget_t* w, int px, int py) {
    if (!w || !(w->flags & UI_STATE_VISIBLE) || !(w->flags & UI_STATE_ENABLED)) return NULL;

    int ax, ay, aw, ah;
    get_absolute_bounds(w, &ax, &ay, &aw, &ah);

    if (px < ax || py < ay || px >= ax + aw || py >= ay + ah) {
        return NULL;
    }

    /* Check children in reverse order (topmost first) */
    for (int i = w->num_children - 1; i >= 0; i--) {
        ui_widget_t* hit = hit_test_widget(w->children[i], px, py);
        if (hit) return hit;
    }

    return w;
}

/* External forward rendering routines */
void ui_draw_widget_tree(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);

static void ui_window_render(ui_window_t* win) {
    if (!win || !win->dirty || !win->raw_win) return;

    gfx_surface_t* s = win_buffer(win->raw_win);
    if (!s) return;

    /* Fill background */
    gfx_fill_rect(s, 0, 0, win->width, win->height, win->app->theme.bg_color);

    /* Render all widgets recursively */
    if (win->root) {
        ui_draw_widget_tree(win, win->root, s);
    }

    win_damage(win->raw_win, 0, 0, win->width, win->height);
    win_commit(win->raw_win);
    win->dirty = 0;
}

void ui_window_show(ui_window_t* win) {
    if (!win) return;
    ui_window_render(win);
}

/* External forward key event handlers */
void ui_handle_widget_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action);

int ui_app_run(ui_app_t* app) {
    if (!app || !app->conn) return -1;

    for (int i = 0; i < app->num_windows; i++) {
        if (app->windows[i]) ui_window_render(app->windows[i]);
    }

    while (app->running) {
        win_event_t ev;
        int r = win_event_wait(app->conn, &ev, 16);
        if (r < 0) break;
        if (r == 0) {
            /* Pacing/idle: render dirty windows */
            for (int i = 0; i < app->num_windows; i++) {
                if (app->windows[i] && app->windows[i]->dirty) {
                    ui_window_render(app->windows[i]);
                }
            }
            continue;
        }

        ui_window_t* target_win = NULL;
        for (int i = 0; i < app->num_windows; i++) {
            if (app->windows[i] && app->windows[i]->raw_win == ev.window) {
                target_win = app->windows[i];
                break;
            }
        }
        if (!target_win) continue;

        switch (ev.type) {
            case WIN_EVENT_POINTER_MOVE: {
                int px = ev.pointer.x;
                int py = ev.pointer.y;
                ui_widget_t* hit = hit_test_widget(target_win->root, px, py);

                if (target_win->hovered != hit) {
                    if (target_win->hovered) {
                        target_win->hovered->flags &= ~UI_STATE_HOVERED;
                    }
                    target_win->hovered = hit;
                    if (hit) {
                        hit->flags |= UI_STATE_HOVERED;
                    }
                    target_win->dirty = 1;
                }
                break;
            }

            case WIN_EVENT_POINTER_BUTTON: {
                int px = ev.pointer.x;
                int py = ev.pointer.y;
                int btn = ev.pointer.button;
                int pressed = (ev.pointer.state == 1);

                if (btn == 1) { /* Left click */
                    if (pressed) {
                        ui_widget_t* hit = hit_test_widget(target_win->root, px, py);
                        if (hit) {
                            ui_widget_set_focus(hit);
                            hit->flags |= UI_STATE_PRESSED;
                            target_win->pressed = hit;
                            target_win->dirty = 1;
                        }
                    } else {
                        if (target_win->pressed) {
                            target_win->pressed->flags &= ~UI_STATE_PRESSED;
                            ui_widget_t* hit = hit_test_widget(target_win->root, px, py);
                            if (hit == target_win->pressed && hit->on_click) {
                                hit->on_click(hit, hit->user_data);
                            }
                            target_win->pressed = NULL;
                            target_win->dirty = 1;
                        }
                    }
                }
                break;
            }

            case WIN_EVENT_KEY: {
                if (target_win->focused) {
                    ui_handle_widget_key(target_win->focused, ev.key.keycode, ev.key.unicode, ev.key.modifiers, ev.key.action);
                    target_win->dirty = 1;
                }
                break;
            }

            case WIN_EVENT_CONFIGURE: {
                target_win->width = ev.configure.width;
                target_win->height = ev.configure.height;
                if (target_win->root) {
                    target_win->root->w = target_win->width;
                    target_win->root->h = target_win->height;
                }
                target_win->dirty = 1;
                break;
            }

            case WIN_EVENT_CLOSE_REQUEST: {
                target_win->running = 0;
                app->running = 0;
                break;
            }

            default:
                break;
        }

        if (target_win->dirty) {
            ui_window_render(target_win);
        }
    }

    return 0;
}

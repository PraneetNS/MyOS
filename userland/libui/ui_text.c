/*
 * ui_text.c - Text Input Controls for libui
 *
 * Implements: Single-line TextBox and Multi-line TextArea
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ui_internal.h"

static void get_abs_coords(ui_widget_t* w, int* ax, int* ay) {
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
}

/* -------------------------------------------------------------------------- */
/* TextBox (Single-Line Input)                                                */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_textbox_create(ui_window_t* win, int x, int y, int w, int h, const char* placeholder, ui_text_fn on_text) {
    ui_widget_t* tb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!tb) return NULL;
    tb->type = UI_WIDGET_TEXTBOX;
    tb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    tb->x = x; tb->y = y; tb->w = w; tb->h = h;
    tb->win = win;
    tb->on_text_change = on_text;
    tb->textbox.cursor_pos = 0;
    tb->textbox.sel_start = -1;
    tb->textbox.sel_end = -1;
    if (placeholder) {
        strncpy(tb->textbox.text, placeholder, sizeof(tb->textbox.text) - 1);
        tb->textbox.cursor_pos = strlen(tb->textbox.text);
    }
    return tb;
}

void ui_textbox_set_text(ui_widget_t* w, const char* text) {
    if (!w || w->type != UI_WIDGET_TEXTBOX) return;
    strncpy(w->textbox.text, text ? text : "", sizeof(w->textbox.text) - 1);
    w->textbox.cursor_pos = strlen(w->textbox.text);
    ui_widget_invalidate(w);
}

const char* ui_textbox_get_text(ui_widget_t* w) {
    if (!w || w->type != UI_WIDGET_TEXTBOX) return "";
    return w->textbox.text;
}

/* -------------------------------------------------------------------------- */
/* TextArea (Multi-Line Input)                                                */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_textarea_create(ui_window_t* win, int x, int y, int w, int h, const char* initial_text) {
    ui_widget_t* ta = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!ta) return NULL;
    ta->type = UI_WIDGET_TEXTAREA;
    ta->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    ta->x = x; ta->y = y; ta->w = w; ta->h = h;
    ta->win = win;

    ta->textarea.buf_cap = 4096;
    ta->textarea.buffer = (char*) calloc(ta->textarea.buf_cap, 1);
    if (initial_text) {
        strncpy(ta->textarea.buffer, initial_text, ta->textarea.buf_cap - 1);
        ta->textarea.buf_len = strlen(ta->textarea.buffer);
    }
    return ta;
}

void ui_textarea_set_text(ui_widget_t* w, const char* text) {
    if (!w || w->type != UI_WIDGET_TEXTAREA) return;
    if (!text) text = "";
    int len = strlen(text);
    if (len + 1 > w->textarea.buf_cap) {
        w->textarea.buf_cap = len + 1024;
        w->textarea.buffer = (char*) realloc(w->textarea.buffer, w->textarea.buf_cap);
    }
    strncpy(w->textarea.buffer, text, w->textarea.buf_cap - 1);
    w->textarea.buf_len = len;
    ui_widget_invalidate(w);
}

const char* ui_textarea_get_text(ui_widget_t* w) {
    if (!w || w->type != UI_WIDGET_TEXTAREA) return "";
    return w->textarea.buffer ? w->textarea.buffer : "";
}

/* -------------------------------------------------------------------------- */
/* Keyboard Handler for Text Controls                                         */
/* -------------------------------------------------------------------------- */

void ui_handle_text_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action) {
    if (action != WIN_KEY_PRESS && action != WIN_KEY_REPEAT) return;

    if (w->type == UI_WIDGET_TEXTBOX) {
        int len = strlen(w->textbox.text);
        int pos = w->textbox.cursor_pos;
        if (pos > len) pos = len;

        /* Backspace */
        if (keycode == 14) {
            if (pos > 0) {
                memmove(&w->textbox.text[pos - 1], &w->textbox.text[pos], len - pos + 1);
                w->textbox.cursor_pos--;
                if (w->on_text_change) w->on_text_change(w, w->textbox.text, w->user_data);
                ui_widget_invalidate(w);
            }
            return;
        }

        /* Delete */
        if (keycode == 111) {
            if (pos < len) {
                memmove(&w->textbox.text[pos], &w->textbox.text[pos + 1], len - pos);
                if (w->on_text_change) w->on_text_change(w, w->textbox.text, w->user_data);
                ui_widget_invalidate(w);
            }
            return;
        }

        /* Left Arrow */
        if (keycode == 105) {
            if (pos > 0) w->textbox.cursor_pos--;
            ui_widget_invalidate(w);
            return;
        }

        /* Right Arrow */
        if (keycode == 106) {
            if (pos < len) w->textbox.cursor_pos++;
            ui_widget_invalidate(w);
            return;
        }

        /* Home */
        if (keycode == 102) {
            w->textbox.cursor_pos = 0;
            ui_widget_invalidate(w);
            return;
        }

        /* End */
        if (keycode == 107) {
            w->textbox.cursor_pos = len;
            ui_widget_invalidate(w);
            return;
        }

        /* Printable Characters */
        if (unicode >= 32 && unicode < 127 && len < (int)sizeof(w->textbox.text) - 2) {
            memmove(&w->textbox.text[pos + 1], &w->textbox.text[pos], len - pos + 1);
            w->textbox.text[pos] = (char)unicode;
            w->textbox.cursor_pos++;
            if (w->on_text_change) w->on_text_change(w, w->textbox.text, w->user_data);
            ui_widget_invalidate(w);
            return;
        }
    } else if (w->type == UI_WIDGET_TEXTAREA) {
        int len = w->textarea.buf_len;
        if (keycode == 14) { /* Backspace */
            if (len > 0) {
                w->textarea.buffer[len - 1] = '\0';
                w->textarea.buf_len--;
                ui_widget_invalidate(w);
            }
            return;
        }
        if (keycode == 28) { /* Enter */
            if (len + 2 < w->textarea.buf_cap) {
                w->textarea.buffer[len] = '\n';
                w->textarea.buffer[len + 1] = '\0';
                w->textarea.buf_len++;
                ui_widget_invalidate(w);
            }
            return;
        }
        if (unicode >= 32 && unicode < 127 && len + 2 < w->textarea.buf_cap) {
            w->textarea.buffer[len] = (char)unicode;
            w->textarea.buffer[len + 1] = '\0';
            w->textarea.buf_len++;
            ui_widget_invalidate(w);
            return;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Text Controls Rendering                                                    */
/* -------------------------------------------------------------------------- */

void ui_draw_text_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s) {
    int ax, ay;
    get_abs_coords(w, &ax, &ay);
    ui_theme_t* theme = &win->app->theme;

    if (w->type == UI_WIDGET_TEXTBOX) {
        gfx_fill_rect(s, ax, ay, w->w, w->h, theme->panel_bg);

        uint32_t border_col = (w->flags & UI_STATE_FOCUSED) ? theme->focus_ring : theme->border_color;
        gfx_draw_rect(s, ax, ay, w->w, w->h, border_col);
        if (w->flags & UI_STATE_FOCUSED) {
            gfx_draw_rect(s, ax + 1, ay + 1, w->w - 2, w->h - 2, theme->focus_ring);
        }

        int ty = ay + (w->h - 14) / 2;
        gfx_draw_text_transparent(s, ax + 6, ty, w->textbox.text, theme->text_color);

        /* Draw blinking or static vertical cursor bar */
        if (w->flags & UI_STATE_FOCUSED) {
            int cx = ax + 6 + w->textbox.cursor_pos * 8;
            if (cx < ax + w->w - 4) {
                gfx_draw_line(s, cx, ay + 4, cx, ay + w->h - 5, theme->primary);
                gfx_draw_line(s, cx + 1, ay + 4, cx + 1, ay + w->h - 5, theme->primary);
            }
        }
    } else if (w->type == UI_WIDGET_TEXTAREA) {
        gfx_fill_rect(s, ax, ay, w->w, w->h, theme->panel_bg);
        uint32_t border_col = (w->flags & UI_STATE_FOCUSED) ? theme->focus_ring : theme->border_color;
        gfx_draw_rect(s, ax, ay, w->w, w->h, border_col);

        /* Render lines with line-height 16 */
        int line_y = ay + 6;
        const char* p = w->textarea.buffer ? w->textarea.buffer : "";
        while (*p && line_y < ay + w->h - 16) {
            char line_buf[128];
            int i = 0;
            while (*p && *p != '\n' && i < (int)sizeof(line_buf) - 1) {
                line_buf[i++] = *p++;
            }
            line_buf[i] = '\0';
            if (*p == '\n') p++;

            gfx_draw_text_transparent(s, ax + 6, line_y, line_buf, theme->text_color);
            line_y += 16;
        }

        if (w->flags & UI_STATE_FOCUSED) {
            /* Draw cursor at current end */
            gfx_draw_line(s, ax + 6, line_y, ax + 6, line_y + 12, theme->primary);
        }
    }
}

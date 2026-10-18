/*
 * ui_widgets.c - Standard Controls for libui
 *
 * Implements: Container, Label, Button, Checkbox, Radio, and ProgressBar
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* Container Widget                                                           */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_container_create(ui_window_t* win, int x, int y, int w, int h) {
    ui_widget_t* c = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!c) return NULL;
    c->type = UI_WIDGET_CONTAINER;
    c->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->win = win;
    return c;
}

/* -------------------------------------------------------------------------- */
/* Label Widget                                                               */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_label_create(ui_window_t* win, int x, int y, int w, int h, const char* text) {
    ui_widget_t* lbl = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!lbl) return NULL;
    lbl->type = UI_WIDGET_LABEL;
    lbl->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    lbl->x = x; lbl->y = y; lbl->w = w; lbl->h = h;
    lbl->win = win;
    lbl->label.color = 0xFF0F172A;
    lbl->label.align = 0; /* Left */
    if (text) strncpy(lbl->label.text, text, sizeof(lbl->label.text) - 1);
    return lbl;
}

void ui_label_set_text(ui_widget_t* w, const char* text) {
    if (!w || w->type != UI_WIDGET_LABEL) return;
    strncpy(w->label.text, text ? text : "", sizeof(w->label.text) - 1);
    ui_widget_invalidate(w);
}

void ui_label_set_color(ui_widget_t* w, uint32_t color) {
    if (!w || w->type != UI_WIDGET_LABEL) return;
    w->label.color = color;
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Button Widget                                                              */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_button_create(ui_window_t* win, int x, int y, int w, int h, const char* text, ui_click_fn on_click) {
    ui_widget_t* btn = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!btn) return NULL;
    btn->type = UI_WIDGET_BUTTON;
    btn->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    btn->x = x; btn->y = y; btn->w = w; btn->h = h;
    btn->win = win;
    btn->on_click = on_click;
    if (text) strncpy(btn->button.text, text, sizeof(btn->button.text) - 1);
    return btn;
}

/* -------------------------------------------------------------------------- */
/* Checkbox Widget                                                            */
/* -------------------------------------------------------------------------- */

static void checkbox_click(ui_widget_t* w, void* user_data) {
    (void) user_data;
    w->checkbox.checked = !w->checkbox.checked;
    if (w->checkbox.checked) w->flags |= UI_STATE_CHECKED;
    else w->flags &= ~UI_STATE_CHECKED;
    if (w->on_change) {
        w->on_change(w, w->checkbox.checked, w->user_data);
    }
}

ui_widget_t* ui_checkbox_create(ui_window_t* win, int x, int y, int w, int h, const char* text, int initial_val, ui_change_fn on_change) {
    ui_widget_t* cb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!cb) return NULL;
    cb->type = UI_WIDGET_CHECKBOX;
    cb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    if (initial_val) cb->flags |= UI_STATE_CHECKED;
    cb->x = x; cb->y = y; cb->w = w; cb->h = h;
    cb->win = win;
    cb->checkbox.checked = initial_val ? 1 : 0;
    cb->on_change = on_change;
    cb->on_click = checkbox_click;
    if (text) strncpy(cb->checkbox.text, text, sizeof(cb->checkbox.text) - 1);
    return cb;
}

/* -------------------------------------------------------------------------- */
/* Radio Button Widget                                                        */
/* -------------------------------------------------------------------------- */

static void uncheck_radio_siblings(ui_widget_t* parent, int group_id, ui_widget_t* keep) {
    if (!parent) return;
    for (int i = 0; i < parent->num_children; i++) {
        ui_widget_t* child = parent->children[i];
        if (child->type == UI_WIDGET_RADIO && child->radio.group_id == group_id && child != keep) {
            child->radio.selected = 0;
            child->flags &= ~UI_STATE_CHECKED;
        }
        uncheck_radio_siblings(child, group_id, keep);
    }
}

static void radio_click(ui_widget_t* w, void* user_data) {
    (void) user_data;
    if (w->radio.selected) return;
    w->radio.selected = 1;
    w->flags |= UI_STATE_CHECKED;
    if (w->win && w->win->root) {
        uncheck_radio_siblings(w->win->root, w->radio.group_id, w);
    }
    if (w->on_change) {
        w->on_change(w, 1, w->user_data);
    }
}

ui_widget_t* ui_radio_create(ui_window_t* win, int x, int y, int w, int h, const char* text, int group_id, int initial_val, ui_change_fn on_change) {
    ui_widget_t* r = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!r) return NULL;
    r->type = UI_WIDGET_RADIO;
    r->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    if (initial_val) r->flags |= UI_STATE_CHECKED;
    r->x = x; r->y = y; r->w = w; r->h = h;
    r->win = win;
    r->radio.group_id = group_id;
    r->radio.selected = initial_val ? 1 : 0;
    r->on_change = on_change;
    r->on_click = radio_click;
    if (text) strncpy(r->radio.text, text, sizeof(r->radio.text) - 1);
    return r;
}

/* -------------------------------------------------------------------------- */
/* Progress Bar Widget                                                        */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_progressbar_create(ui_window_t* win, int x, int y, int w, int h, int min_val, int max_val, int initial_val) {
    ui_widget_t* p = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!p) return NULL;
    p->type = UI_WIDGET_PROGRESS;
    p->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    p->x = x; p->y = y; p->w = w; p->h = h;
    p->win = win;
    p->progress.min_val = min_val;
    p->progress.max_val = (max_val > min_val) ? max_val : (min_val + 100);
    p->progress.cur_val = initial_val;
    return p;
}

void ui_progressbar_set_value(ui_widget_t* w, int val) {
    if (!w || w->type != UI_WIDGET_PROGRESS) return;
    if (val < w->progress.min_val) val = w->progress.min_val;
    if (val > w->progress.max_val) val = w->progress.max_val;
    w->progress.cur_val = val;
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Standard Controls Rendering                                                */
/* -------------------------------------------------------------------------- */

void ui_draw_standard_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s) {
    int ax, ay;
    get_abs_coords(w, &ax, &ay);
    ui_theme_t* theme = &win->app->theme;

    switch (w->type) {
        case UI_WIDGET_CONTAINER:
            /* Transparent container: nothing to draw */
            break;

        case UI_WIDGET_LABEL: {
            int tx = ax;
            int ty = ay + (w->h - 14) / 2;
            if (ty < ay) ty = ay;
            gfx_draw_text_transparent(s, tx, ty, w->label.text, w->label.color);
            break;
        }

        case UI_WIDGET_BUTTON: {
            uint32_t bg = theme->primary;
            if (w->flags & UI_STATE_PRESSED) bg = theme->primary_active;
            else if (w->flags & UI_STATE_HOVERED) bg = theme->primary_hover;

            gfx_fill_rect(s, ax, ay, w->w, w->h, bg);
            gfx_draw_rect(s, ax, ay, w->w, w->h, theme->border_color);

            /* Focus indicator */
            if (w->flags & UI_STATE_FOCUSED) {
                gfx_draw_rect(s, ax + 2, ay + 2, w->w - 4, w->h - 4, theme->focus_ring);
            }

            int text_w = strlen(w->button.text) * 8;
            int tx = ax + (w->w - text_w) / 2;
            int ty = ay + (w->h - 14) / 2;
            if (tx < ax + 4) tx = ax + 4;
            gfx_draw_text_transparent(s, tx, ty, w->button.text, 0xFFFFFFFF);
            break;
        }

        case UI_WIDGET_CHECKBOX: {
            int box_size = 16;
            int by = ay + (w->h - box_size) / 2;
            uint32_t box_bg = (w->checkbox.checked) ? theme->primary : theme->panel_bg;

            gfx_fill_rect(s, ax, by, box_size, box_size, box_bg);
            gfx_draw_rect(s, ax, by, box_size, box_size, theme->border_color);

            if (w->checkbox.checked) {
                /* Draw white checkmark 'x' */
                gfx_draw_text_transparent(s, ax + 4, by + 1, "x", 0xFFFFFFFF);
            }

            int tx = ax + box_size + 8;
            int ty = ay + (w->h - 14) / 2;
            gfx_draw_text_transparent(s, tx, ty, w->checkbox.text, theme->text_color);
            break;
        }

        case UI_WIDGET_RADIO: {
            int r_size = 16;
            int ry = ay + (w->h - r_size) / 2;

            gfx_fill_rect(s, ax, ry, r_size, r_size, theme->panel_bg);
            gfx_draw_rect(s, ax, ry, r_size, r_size, theme->border_color);

            if (w->radio.selected) {
                /* Inner dot */
                gfx_fill_rect(s, ax + 4, ry + 4, r_size - 8, r_size - 8, theme->primary);
            }

            int tx = ax + r_size + 8;
            int ty = ay + (w->h - 14) / 2;
            gfx_draw_text_transparent(s, tx, ty, w->radio.text, theme->text_color);
            break;
        }

        case UI_WIDGET_PROGRESS: {
            int range = w->progress.max_val - w->progress.min_val;
            if (range <= 0) range = 100;
            int val = w->progress.cur_val - w->progress.min_val;
            int fill_w = (val * (w->w - 2)) / range;
            if (fill_w < 0) fill_w = 0;
            if (fill_w > w->w - 2) fill_w = w->w - 2;

            /* Trough */
            gfx_fill_rect(s, ax, ay, w->w, w->h, 0xFFE2E8F0);
            gfx_draw_rect(s, ax, ay, w->w, w->h, theme->border_color);

            /* Fill */
            if (fill_w > 0) {
                gfx_fill_rect(s, ax + 1, ay + 1, fill_w, w->h - 2, theme->primary);
            }

            char pct[16];
            int p = (val * 100) / range;
            snprintf(pct, sizeof(pct), "%d%%", p);
            int tx = ax + (w->w - strlen(pct) * 8) / 2;
            int ty = ay + (w->h - 14) / 2;
            gfx_draw_text_transparent(s, tx, ty, pct, (fill_w > (w->w / 2)) ? 0xFFFFFFFF : theme->text_color);
            break;
        }

        default:
            break;
    }
}

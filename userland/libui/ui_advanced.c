/*
 * ui_advanced.c - Advanced Controls & Master Dispatcher for libui
 *
 * Implements: Listbox, Scrollbar, Tabs, Menubar, Dialogs,
 * and unified rendering / key dispatchers.
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
/* ListBox Widget                                                             */
/* -------------------------------------------------------------------------- */

static void listbox_click(ui_widget_t* w, void* user_data) {
    (void) user_data;
    /* Click handler calculation occurs in button press */
}

ui_widget_t* ui_listbox_create(ui_window_t* win, int x, int y, int w, int h, ui_change_fn on_select) {
    ui_widget_t* lb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!lb) return NULL;
    lb->type = UI_WIDGET_LISTBOX;
    lb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    lb->x = x; lb->y = y; lb->w = w; lb->h = h;
    lb->win = win;
    if (win) lb->a11y_id = win->next_a11y_id++;
    lb->on_change = on_select;
    lb->on_click = listbox_click;
    lb->listbox.cap_items = 16;
    lb->listbox.items = (char**) calloc(lb->listbox.cap_items, sizeof(char*));
    lb->listbox.selected_idx = -1;
    return lb;
}

void ui_listbox_add_item(ui_widget_t* w, const char* item) {
    if (!w || w->type != UI_WIDGET_LISTBOX || !item) return;
    if (w->listbox.num_items >= w->listbox.cap_items) {
        w->listbox.cap_items *= 2;
        w->listbox.items = (char**) realloc(w->listbox.items, w->listbox.cap_items * sizeof(char*));
    }
    w->listbox.items[w->listbox.num_items++] = strdup(item);
    if (w->listbox.selected_idx < 0) w->listbox.selected_idx = 0;
    ui_widget_invalidate(w);
}

void ui_listbox_clear(ui_widget_t* w) {
    if (!w || w->type != UI_WIDGET_LISTBOX) return;
    for (int i = 0; i < w->listbox.num_items; i++) {
        if (w->listbox.items[i]) free(w->listbox.items[i]);
    }
    w->listbox.num_items = 0;
    w->listbox.selected_idx = -1;
    ui_widget_invalidate(w);
}

const char* ui_listbox_get_selected(ui_widget_t* w) {
    if (!w || w->type != UI_WIDGET_LISTBOX) return NULL;
    if (w->listbox.selected_idx >= 0 && w->listbox.selected_idx < w->listbox.num_items) {
        return w->listbox.items[w->listbox.selected_idx];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Scrollbar Widget                                                           */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_scrollbar_create(ui_window_t* win, int x, int y, int w, int h, int is_vertical, ui_change_fn on_scroll) {
    ui_widget_t* sb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!sb) return NULL;
    sb->type = UI_WIDGET_SCROLLBAR;
    sb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    sb->x = x; sb->y = y; sb->w = w; sb->h = h;
    sb->win = win;
    if (win) sb->a11y_id = win->next_a11y_id++;
    sb->on_change = on_scroll;
    sb->scrollbar.is_vertical = is_vertical;
    sb->scrollbar.min_val = 0;
    sb->scrollbar.max_val = 100;
    sb->scrollbar.cur_val = 0;
    sb->scrollbar.page_size = 20;
    return sb;
}

/* -------------------------------------------------------------------------- */
/* Tabs Widget                                                                */
/* -------------------------------------------------------------------------- */

static void tabs_click(ui_widget_t* w, void* user_data) {
    (void) user_data;
    /* Calculated on click */
}

ui_widget_t* ui_tabs_create(ui_window_t* win, int x, int y, int w, int h) {
    ui_widget_t* tb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!tb) return NULL;
    tb->type = UI_WIDGET_TABS;
    tb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    tb->x = x; tb->y = y; tb->w = w; tb->h = h;
    tb->win = win;
    if (win) tb->a11y_id = win->next_a11y_id++;
    tb->on_click = tabs_click;
    tb->tabs.active_tab = 0;
    return tb;
}

void ui_tabs_add_tab(ui_widget_t* tabs, const char* title, ui_widget_t* panel) {
    if (!tabs || tabs->type != UI_WIDGET_TABS || tabs->tabs.num_tabs >= 8) return;
    int idx = tabs->tabs.num_tabs++;
    strncpy(tabs->tabs.tab_names[idx], title ? title : "Tab", sizeof(tabs->tabs.tab_names[idx]) - 1);
    tabs->tabs.tab_panels[idx] = panel;
    ui_widget_add_child(tabs, panel);

    /* Hide non-active tab panels */
    if (idx != tabs->tabs.active_tab && panel) {
        panel->flags &= ~UI_STATE_VISIBLE;
    }
    ui_widget_invalidate(tabs);
}

/* -------------------------------------------------------------------------- */
/* Menubar Widget                                                             */
/* -------------------------------------------------------------------------- */

ui_widget_t* ui_menubar_create(ui_window_t* win, int x, int y, int w, int h) {
    ui_widget_t* mb = (ui_widget_t*) calloc(1, sizeof(ui_widget_t));
    if (!mb) return NULL;
    mb->type = UI_WIDGET_MENUBAR;
    mb->flags = UI_STATE_VISIBLE | UI_STATE_ENABLED;
    mb->x = x; mb->y = y; mb->w = w; mb->h = h;
    mb->win = win;
    if (win) mb->a11y_id = win->next_a11y_id++;
    mb->menubar.open_menu = -1;
    return mb;
}

void ui_menubar_add_menu(ui_widget_t* mb, const char* title) {
    if (!mb || mb->type != UI_WIDGET_MENUBAR || mb->menubar.num_menus >= 8) return;
    int idx = mb->menubar.num_menus++;
    strncpy(mb->menubar.menu_titles[idx], title ? title : "Menu", sizeof(mb->menubar.menu_titles[idx]) - 1);
    mb->menubar.num_items[idx] = 0;
}

void ui_menubar_add_item(ui_widget_t* mb, int menu_idx, const char* label, ui_click_fn cb) {
    if (!mb || mb->type != UI_WIDGET_MENUBAR || menu_idx < 0 || menu_idx >= mb->menubar.num_menus) return;
    int item_idx = mb->menubar.num_items[menu_idx];
    if (item_idx >= 16) return;
    strncpy(mb->menubar.item_labels[menu_idx][item_idx], label ? label : "Item", sizeof(mb->menubar.item_labels[menu_idx][item_idx]) - 1);
    mb->menubar.item_callbacks[menu_idx][item_idx] = cb;
    mb->menubar.num_items[menu_idx]++;
}

/* -------------------------------------------------------------------------- */
/* Message Box Dialog                                                         */
/* -------------------------------------------------------------------------- */

static void dialog_ok_clicked(ui_widget_t* btn, void* user_data) {
    ui_window_t* dlg = (ui_window_t*) user_data;
    if (dlg) {
        ui_window_destroy(dlg);
    }
}

void ui_dialog_message_box(ui_window_t* parent, const char* title, const char* message) {
    if (!parent) return;
    ui_window_t* dlg = ui_window_create(parent->app, 320, 160, title ? title : "Message");
    if (!dlg) return;

    ui_label_create(dlg, 20, 20, 280, 50, message ? message : "");
    ui_button_create(dlg, 110, 100, 100, 32, "OK", dialog_ok_clicked);
    dlg->root->children[1]->user_data = dlg;
    ui_window_show(dlg);
}

/* -------------------------------------------------------------------------- */
/* Advanced Controls Rendering                                                */
/* -------------------------------------------------------------------------- */

void ui_draw_advanced_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s) {
    int ax, ay;
    get_abs_coords(w, &ax, &ay);
    ui_theme_t* theme = &win->app->theme;

    if (w->type == UI_WIDGET_LISTBOX) {
        gfx_fill_rect(s, ax, ay, w->w, w->h, theme->panel_bg);
        gfx_draw_rect(s, ax, ay, w->w, w->h, theme->border_color);

        int item_h = 22;
        int max_visible = w->h / item_h;
        for (int i = 0; i < max_visible && i < w->listbox.num_items; i++) {
            int iy = ay + i * item_h;
            int is_selected = (i == w->listbox.selected_idx);

            if (is_selected) {
                gfx_fill_rect(s, ax + 1, iy, w->w - 2, item_h, theme->selection_bg);
            }

            uint32_t text_col = is_selected ? 0xFF1E3A8A : theme->text_color;
            gfx_draw_text_transparent(s, ax + 8, iy + 4, w->listbox.items[i], text_col);
        }
    } else if (w->type == UI_WIDGET_TABS) {
        int tab_h = 28;
        int cur_x = ax;

        /* Tab Header Bar */
        gfx_fill_rect(s, ax, ay, w->w, tab_h, 0xFFE2E8F0);
        gfx_draw_rect(s, ax, ay, w->w, w->h, theme->border_color);

        for (int i = 0; i < w->tabs.num_tabs; i++) {
            int tw = strlen(w->tabs.tab_names[i]) * 8 + 24;
            int is_active = (i == w->tabs.active_tab);

            uint32_t bg = is_active ? theme->panel_bg : 0xFFCBD5E1;
            gfx_fill_rect(s, cur_x, ay, tw, tab_h, bg);
            gfx_draw_rect(s, cur_x, ay, tw, tab_h, theme->border_color);

            if (is_active) {
                /* Remove bottom border of active tab to connect with panel */
                gfx_draw_line(s, cur_x + 1, ay + tab_h - 1, cur_x + tw - 2, ay + tab_h - 1, theme->panel_bg);
                /* Accent top bar */
                gfx_fill_rect(s, cur_x, ay, tw, 3, theme->primary);
            }

            gfx_draw_text_transparent(s, cur_x + 12, ay + 8, w->tabs.tab_names[i], is_active ? theme->primary : theme->text_color);
            cur_x += tw + 2;
        }
    } else if (w->type == UI_WIDGET_MENUBAR) {
        gfx_fill_rect(s, ax, ay, w->w, w->h, 0xFFF8FAFC);
        gfx_draw_rect(s, ax, ay, w->w, w->h, theme->border_color);

        int cur_x = ax + 10;
        for (int i = 0; i < w->menubar.num_menus; i++) {
            int tw = strlen(w->menubar.menu_titles[i]) * 8 + 16;
            gfx_draw_text_transparent(s, cur_x + 8, ay + 6, w->menubar.menu_titles[i], theme->text_color);
            cur_x += tw;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Master Unified Draw & Key Dispatchers                                      */
/* -------------------------------------------------------------------------- */

void ui_draw_standard_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);
void ui_draw_text_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);
void ui_handle_text_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action);

void ui_draw_widget_tree(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s) {
    if (!w || !(w->flags & UI_STATE_VISIBLE)) return;

    /* Render current widget */
    if (w->custom_draw) {
        w->custom_draw(w, s);
    } else {
        ui_draw_standard_widgets(win, w, s);
        ui_draw_text_widgets(win, w, s);
        ui_draw_advanced_widgets(win, w, s);
    }

    /* Render children */
    for (int i = 0; i < w->num_children; i++) {
        ui_draw_widget_tree(win, w->children[i], s);
    }
}

void ui_handle_widget_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action) {
    if (!w) return;
    ui_handle_text_key(w, keycode, unicode, mods, action);
}

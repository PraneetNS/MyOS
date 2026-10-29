#ifndef _MYOS_UI_INTERNAL_H_
#define _MYOS_UI_INTERNAL_H_

#include "ui.h"

struct ui_window {
    ui_app_t* app;
    win_t* raw_win;
    int width;
    int height;
    char title[128];
    ui_widget_t* root;
    ui_widget_t* focused;
    ui_widget_t* hovered;
    ui_widget_t* pressed;
    uint32_t next_a11y_id;
    int dirty;
    int running;
};

struct ui_app {
    win_conn_t* conn;
    ui_window_t** windows;
    int num_windows;
    int cap_windows;
    int running;
    ui_theme_t theme;
};

void ui_draw_widget_tree(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);
void ui_draw_standard_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);
void ui_draw_text_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);
void ui_draw_advanced_widgets(ui_window_t* win, ui_widget_t* w, gfx_surface_t* s);

void ui_handle_widget_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action);
void ui_handle_text_key(ui_widget_t* w, uint32_t keycode, uint32_t unicode, uint32_t mods, uint32_t action);

void ui_a11y_on_get_tree(win_t* win, uint32_t root_id, uint32_t max_depth, void* udata);
void ui_a11y_on_action(win_t* win, uint32_t node_id, const char* action, const char* args, void* udata);

#endif

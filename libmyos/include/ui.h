/*
 * ui.h - MyOS Widget Toolkit (libui) Public API
 *
 * Stage 18B-2 Step 3
 * Retained-lite widget toolkit built on libwin and libgfx.
 */

#ifndef _MYOS_UI_H_
#define _MYOS_UI_H_

#include <stdint.h>
#include <stddef.h>
#include <gfx.h>
#include <win.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Widget Types */
typedef enum {
    UI_WIDGET_CONTAINER = 0,
    UI_WIDGET_LABEL,
    UI_WIDGET_BUTTON,
    UI_WIDGET_CHECKBOX,
    UI_WIDGET_RADIO,
    UI_WIDGET_PROGRESS,
    UI_WIDGET_TEXTBOX,
    UI_WIDGET_TEXTAREA,
    UI_WIDGET_LISTBOX,
    UI_WIDGET_SCROLLBAR,
    UI_WIDGET_TABS,
    UI_WIDGET_MENUBAR,
    UI_WIDGET_CUSTOM
} ui_widget_type_t;

/* Widget State Flags */
#define UI_STATE_VISIBLE   (1 << 0)
#define UI_STATE_ENABLED   (1 << 1)
#define UI_STATE_FOCUSED   (1 << 2)
#define UI_STATE_HOVERED   (1 << 3)
#define UI_STATE_PRESSED   (1 << 4)
#define UI_STATE_CHECKED   (1 << 5)

/* Forward Declarations */
typedef struct ui_app ui_app_t;
typedef struct ui_window ui_window_t;
typedef struct ui_widget ui_widget_t;

/* Widget Event Callback Signatures */
typedef void (*ui_click_fn)(ui_widget_t* w, void* user_data);
typedef void (*ui_change_fn)(ui_widget_t* w, int new_val, void* user_data);
typedef void (*ui_text_fn)(ui_widget_t* w, const char* text, void* user_data);
typedef void (*ui_custom_draw_fn)(ui_widget_t* w, gfx_surface_t* s);

/* UI Theme Colors */
typedef struct {
    uint32_t bg_color;         /* Window background #F1F5F9 */
    uint32_t panel_bg;         /* Card/Panel background #FFFFFF */
    uint32_t text_color;       /* Primary text #0F172A */
    uint32_t text_secondary;   /* Secondary text #64748B */
    uint32_t primary;          /* Primary accent #2563EB */
    uint32_t primary_hover;    /* Accent hover #1D4ED8 */
    uint32_t primary_active;   /* Accent active #1E40AF */
    uint32_t border_color;     /* Normal border #CBD5E1 */
    uint32_t focus_ring;       /* Focus outline #38BDF8 */
    uint32_t selection_bg;     /* Text/list selection #93C5FD */
} ui_theme_t;

/* Generic Widget Structure */
struct ui_widget {
    ui_widget_type_t type;
    uint32_t flags;
    uint32_t a11y_id;
    int is_secret;
    int x, y, w, h;            /* Relative to parent */
    ui_window_t* win;
    ui_widget_t* parent;
    ui_widget_t** children;
    int num_children;
    int cap_children;

    void* user_data;
    ui_click_fn on_click;
    ui_change_fn on_change;
    ui_text_fn on_text_change;
    ui_custom_draw_fn custom_draw;

    /* Widget Specific Data */
    union {
        struct {
            char text[256];
            uint32_t color;
            int align; /* 0=left, 1=center, 2=right */
        } label;

        struct {
            char text[128];
            int is_default;
        } button;

        struct {
            char text[128];
            int checked;
        } checkbox;

        struct {
            char text[128];
            int group_id;
            int selected;
        } radio;

        struct {
            int min_val;
            int max_val;
            int cur_val;
        } progress;

        struct {
            char text[512];
            int cursor_pos;
            int sel_start;
            int sel_end;
            int scroll_offset;
        } textbox;

        struct {
            char* buffer;
            int buf_len;
            int buf_cap;
            int cursor_line;
            int cursor_col;
            int scroll_top;
        } textarea;

        struct {
            char** items;
            int num_items;
            int cap_items;
            int selected_idx;
            int scroll_top;
        } listbox;

        struct {
            int is_vertical;
            int min_val;
            int max_val;
            int cur_val;
            int page_size;
        } scrollbar;

        struct {
            char tab_names[8][32];
            ui_widget_t* tab_panels[8];
            int num_tabs;
            int active_tab;
        } tabs;

        struct {
            char menu_titles[8][32];
            char item_labels[8][16][32];
            ui_click_fn item_callbacks[8][16];
            int num_items[8];
            int num_menus;
            int open_menu;
        } menubar;
    };
};

/* Application & Window Management */
ui_app_t*     ui_app_create(void);
void          ui_app_destroy(ui_app_t* app);
int           ui_app_run(ui_app_t* app);
void          ui_app_quit(ui_app_t* app);

ui_window_t*  ui_window_create(ui_app_t* app, int width, int height, const char* title);
void          ui_window_destroy(ui_window_t* win);
void          ui_window_show(ui_window_t* win);
void          ui_window_invalidate(ui_window_t* win);
win_t*        ui_window_get_raw(ui_window_t* win);
ui_widget_t*  ui_window_get_root(ui_window_t* win);
ui_theme_t*   ui_theme_get_default(void);

/* Base Widget Operations */
void          ui_widget_add_child(ui_widget_t* parent, ui_widget_t* child);
void          ui_widget_set_focus(ui_widget_t* w);
void          ui_widget_invalidate(ui_widget_t* w);

/* Widget Constructors */
ui_widget_t*  ui_container_create(ui_window_t* win, int x, int y, int w, int h);
ui_widget_t*  ui_label_create(ui_window_t* win, int x, int y, int w, int h, const char* text);
ui_widget_t*  ui_button_create(ui_window_t* win, int x, int y, int w, int h, const char* text, ui_click_fn on_click);
ui_widget_t*  ui_checkbox_create(ui_window_t* win, int x, int y, int w, int h, const char* text, int initial_val, ui_change_fn on_change);
ui_widget_t*  ui_radio_create(ui_window_t* win, int x, int y, int w, int h, const char* text, int group_id, int initial_val, ui_change_fn on_change);
ui_widget_t*  ui_progressbar_create(ui_window_t* win, int x, int y, int w, int h, int min_val, int max_val, int initial_val);
ui_widget_t*  ui_textbox_create(ui_window_t* win, int x, int y, int w, int h, const char* placeholder, ui_text_fn on_text);
ui_widget_t*  ui_textarea_create(ui_window_t* win, int x, int y, int w, int h, const char* initial_text);
ui_widget_t*  ui_listbox_create(ui_window_t* win, int x, int y, int w, int h, ui_change_fn on_select);
ui_widget_t*  ui_scrollbar_create(ui_window_t* win, int x, int y, int w, int h, int is_vertical, ui_change_fn on_scroll);
ui_widget_t*  ui_tabs_create(ui_window_t* win, int x, int y, int w, int h);
ui_widget_t*  ui_menubar_create(ui_window_t* win, int x, int y, int w, int h);

/* Widget Helper Operations */
void          ui_label_set_text(ui_widget_t* w, const char* text);
void          ui_label_set_color(ui_widget_t* w, uint32_t color);
void          ui_progressbar_set_value(ui_widget_t* w, int val);
void          ui_listbox_add_item(ui_widget_t* w, const char* item);
void          ui_listbox_clear(ui_widget_t* w);
const char*   ui_listbox_get_selected(ui_widget_t* w);
void          ui_tabs_add_tab(ui_widget_t* tabs, const char* title, ui_widget_t* panel);
void          ui_menubar_add_menu(ui_widget_t* mb, const char* title);
void          ui_menubar_add_item(ui_widget_t* mb, int menu_idx, const char* label, ui_click_fn cb);
void          ui_textbox_set_text(ui_widget_t* w, const char* text);
const char*   ui_textbox_get_text(ui_widget_t* w);
void          ui_textarea_set_text(ui_widget_t* w, const char* text);
const char*   ui_textarea_get_text(ui_widget_t* w);

/* Dialog Helpers */
void          ui_dialog_message_box(ui_window_t* parent, const char* title, const char* message);

/* Accessibility Helpers */
void          ui_widget_set_secret(ui_widget_t* w, int is_secret);
uint32_t      ui_widget_get_a11y_id(ui_widget_t* w);
ui_widget_t*  ui_window_find_by_a11y_id(ui_window_t* win, uint32_t a11y_id);

#ifdef __cplusplus
}
#endif

#endif /* _MYOS_UI_H_ */

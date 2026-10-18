/*
 * uidemo.c - MyOS Widget Toolkit Showcase Application (libui)
 *
 * Stage 18B-2 Step 3
 * Demonstrates: buttons, checkboxes, radio groups, progress bar,
 * single-line text input, multiline text area, listbox, tabs,
 * menubar, and dialog message box.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ui.h"

static ui_widget_t* g_status_lbl = NULL;
static ui_widget_t* g_pbar = NULL;
static int g_pbar_val = 40;

static void on_btn_click(ui_widget_t* w, void* data) {
    (void) data;
    printf("[uidemo] Button '%s' clicked\n", w->button.text);
    fflush(stdout);
    if (g_status_lbl) {
        ui_label_set_text(g_status_lbl, "Status: Action Button Clicked!");
    }
    g_pbar_val = (g_pbar_val + 20);
    if (g_pbar_val > 100) g_pbar_val = 0;
    if (g_pbar) {
        ui_progressbar_set_value(g_pbar, g_pbar_val);
    }
}

static void on_cb_change(ui_widget_t* w, int val, void* data) {
    (void) data;
    printf("[uidemo] Checkbox '%s' changed: %d\n", w->checkbox.text, val);
    fflush(stdout);
    if (g_status_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Status: Checkbox %s", val ? "Checked" : "Unchecked");
        ui_label_set_text(g_status_lbl, buf);
    }
}

static void on_radio_change(ui_widget_t* w, int val, void* data) {
    (void) data;
    printf("[uidemo] Radio '%s' selected: %d\n", w->radio.text, val);
    fflush(stdout);
    if (g_status_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Status: Selected %s", w->radio.text);
        ui_label_set_text(g_status_lbl, buf);
    }
}

static void on_text_changed(ui_widget_t* w, const char* text, void* data) {
    (void) data;
    printf("[uidemo] TextBox content: '%s'\n", text);
    fflush(stdout);
}

static void on_list_select(ui_widget_t* w, int val, void* data) {
    (void) data; (void) val;
    const char* sel = ui_listbox_get_selected(w);
    printf("[uidemo] Listbox selected: '%s'\n", sel ? sel : "(none)");
    fflush(stdout);
    if (g_status_lbl && sel) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Selected: %s", sel);
        ui_label_set_text(g_status_lbl, buf);
    }
}

static void on_menu_exit(ui_widget_t* w, void* data) {
    (void) w;
    ui_app_t* app = (ui_app_t*) data;
    if (app) ui_app_quit(app);
}

static void on_dialog_trigger(ui_widget_t* w, void* data) {
    (void) w;
    ui_window_t* win = (ui_window_t*) data;
    printf("[uidemo] Triggering modal dialog\n");
    fflush(stdout);
    ui_dialog_message_box(win, "About MyOS libui", "MyOS Widget Toolkit (Stage 18B-2 Step 3)\nFully functional retained UI system.");
}

int main(int argc, char** argv) {
    (void) argc; (void) argv;

    printf("[uidemo] Initializing libui application...\n");
    fflush(stdout);

    ui_app_t* app = ui_app_create();
    if (!app) {
        fprintf(stderr, "[uidemo] Error: cannot connect to display server\n");
        return 1;
    }

    ui_window_t* win = ui_window_create(app, 620, 440, "MyOS UI Widget Showcase");
    if (!win) {
        fprintf(stderr, "[uidemo] Error: failed to create UI window\n");
        ui_app_destroy(app);
        return 1;
    }

    ui_widget_t* root = ui_window_get_root(win);

    /* 1. Menubar */
    ui_widget_t* mb = ui_menubar_create(win, 0, 0, 620, 26);
    ui_menubar_add_menu(mb, "File");
    ui_menubar_add_item(mb, 0, "Exit", on_menu_exit);
    ui_menubar_add_menu(mb, "Help");
    ui_widget_add_child(root, mb);

    /* 2. Tabs Container */
    ui_widget_t* tabs = ui_tabs_create(win, 15, 38, 590, 350);
    ui_widget_add_child(root, tabs);

    /* --- Tab 1: Basic Controls --- */
    ui_widget_t* tab1 = ui_container_create(win, 0, 30, 590, 320);
    ui_widget_t* l1 = ui_label_create(win, 20, 15, 200, 20, "Interactive Button Controls:");
    ui_widget_add_child(tab1, l1);

    ui_widget_t* btn1 = ui_button_create(win, 20, 40, 140, 32, "Click Action", on_btn_click);
    ui_widget_add_child(tab1, btn1);

    ui_widget_t* btn_dlg = ui_button_create(win, 180, 40, 140, 32, "Show Dialog", on_dialog_trigger);
    btn_dlg->user_data = win;
    ui_widget_add_child(tab1, btn_dlg);

    ui_widget_t* l2 = ui_label_create(win, 20, 85, 200, 20, "Checkboxes & Options:");
    ui_widget_add_child(tab1, l2);

    ui_widget_t* cb1 = ui_checkbox_create(win, 20, 110, 200, 24, "Enable Hardware Accel", 1, on_cb_change);
    ui_widget_add_child(tab1, cb1);

    ui_widget_t* cb2 = ui_checkbox_create(win, 20, 140, 200, 24, "Smooth Font Anti-aliasing", 0, on_cb_change);
    ui_widget_add_child(tab1, cb2);

    ui_widget_t* l3 = ui_label_create(win, 300, 85, 200, 20, "Radio Options (Theme):");
    ui_widget_add_child(tab1, l3);

    ui_widget_t* r1 = ui_radio_create(win, 300, 110, 150, 24, "Slate Dark Mode", 1, 1, on_radio_change);
    ui_widget_add_child(tab1, r1);

    ui_widget_t* r2 = ui_radio_create(win, 300, 140, 150, 24, "Ocean Blue Light", 1, 0, on_radio_change);
    ui_widget_add_child(tab1, r2);

    ui_widget_t* l4 = ui_label_create(win, 20, 185, 200, 20, "System Progress Bar:");
    ui_widget_add_child(tab1, l4);

    g_pbar = ui_progressbar_create(win, 20, 210, 540, 24, 0, 100, g_pbar_val);
    ui_widget_add_child(tab1, g_pbar);

    ui_tabs_add_tab(tabs, "Controls", tab1);

    /* --- Tab 2: Text Inputs --- */
    ui_widget_t* tab2 = ui_container_create(win, 0, 30, 590, 320);
    ui_widget_t* l5 = ui_label_create(win, 20, 15, 250, 20, "Single-line Input (TextBox):");
    ui_widget_add_child(tab2, l5);

    ui_widget_t* tb = ui_textbox_create(win, 20, 40, 540, 32, "Hello MyOS libui!", on_text_changed);
    ui_widget_add_child(tab2, tb);

    ui_widget_t* l6 = ui_label_create(win, 20, 85, 250, 20, "Multi-line Input (TextArea):");
    ui_widget_add_child(tab2, l6);

    ui_widget_t* ta = ui_textarea_create(win, 20, 110, 540, 140, "Welcome to MyOS Stage 18B-2.\nWidgets are rendered in RAM backbuffer.\nSupports line breaks and fast blitting.");
    ui_widget_add_child(tab2, ta);

    ui_tabs_add_tab(tabs, "Text Inputs", tab2);

    /* --- Tab 3: List & Items --- */
    ui_widget_t* tab3 = ui_container_create(win, 0, 30, 590, 320);
    ui_widget_t* l7 = ui_label_create(win, 20, 15, 250, 20, "Scrollable List View:");
    ui_widget_add_child(tab3, l7);

    ui_widget_t* lb = ui_listbox_create(win, 20, 40, 300, 200, on_list_select);
    ui_listbox_add_item(lb, "kernel32.bin (Stage 18B)");
    ui_listbox_add_item(lb, "libwin.a (Display Client)");
    ui_listbox_add_item(lb, "libui.a (Widget Toolkit)");
    ui_listbox_add_item(lb, "term.elf (Terminal)");
    ui_listbox_add_item(lb, "files.elf (File Manager)");
    ui_listbox_add_item(lb, "edit.elf (Text Editor)");
    ui_listbox_add_item(lb, "sysmon.elf (Task Manager)");
    ui_widget_add_child(tab3, lb);

    ui_tabs_add_tab(tabs, "List View", tab3);

    /* 3. Bottom Status Bar */
    g_status_lbl = ui_label_create(win, 20, 400, 580, 24, "Status: Ready");
    ui_label_set_color(g_status_lbl, 0xFF475569);
    ui_widget_add_child(root, g_status_lbl);

    ui_window_show(win);
    printf("[uidemo] UI demo window running\n");
    fflush(stdout);

    int ret = ui_app_run(app);
    ui_app_destroy(app);
    printf("[uidemo] Exited cleanly.\n");
    return ret;
}

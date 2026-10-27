/*
 * files.c - Graphical File Manager for MyOS
 *
 * Stage 18B-2 Step 5a
 * Built using libui (Widget Toolkit)
 * Features:
 * - Directory navigation with path breadcrumbs
 * - Up / Home / Refresh buttons
 * - LFN support with directory contents listing ([DIR] / [FILE])
 * - File selection, item counter, and status bar
 * - Open action for folder navigation and file inspection
 * - Delete action for file removal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ui.h>

#define MAX_PATH_LEN 256
#define MAX_ENTRIES  128

typedef struct {
    char name[128];
    int is_dir;
    uint32_t size;
} file_item_t;

static char s_current_dir[MAX_PATH_LEN] = "/";
static file_item_t s_entries[MAX_ENTRIES];
static int s_num_entries = 0;

static ui_app_t*    g_app = NULL;
static ui_window_t* g_win = NULL;
static ui_widget_t* g_path_box = NULL;
static ui_widget_t* g_listbox = NULL;
static ui_widget_t* g_status_lbl = NULL;

static void refresh_directory(const char* dir_path);

static int compare_entries(const void* a, const void* b) {
    const file_item_t* fa = (const file_item_t*)a;
    const file_item_t* fb = (const file_item_t*)b;
    if (fa->is_dir != fb->is_dir) {
        return fb->is_dir - fa->is_dir; /* Directories first */
    }
    return strcmp(fa->name, fb->name);
}

static void load_directory(const char* path) {
    s_num_entries = 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return;
    }

    struct dirent de;
    while (getdents(fd, &de, sizeof(de)) > 0 && s_num_entries < MAX_ENTRIES) {
        if (de.d_name[0] == '\0') continue;
        if (strcmp(de.d_name, ".") == 0) continue;

        file_item_t* item = &s_entries[s_num_entries++];
        strncpy(item->name, de.d_name, sizeof(item->name) - 1);
        item->name[sizeof(item->name) - 1] = '\0';
        item->is_dir = (de.d_type == 2); /* DT_DIR = 2 */
        item->size = de.d_size;
    }
    close(fd);

    if (s_num_entries > 1) {
        qsort(s_entries, s_num_entries, sizeof(file_item_t), compare_entries);
    }
}

static void refresh_directory(const char* dir_path) {
    strncpy(s_current_dir, dir_path, sizeof(s_current_dir) - 1);
    s_current_dir[sizeof(s_current_dir) - 1] = '\0';

    /* Normalize root */
    if (s_current_dir[0] == '\0') {
        strcpy(s_current_dir, "/");
    }

    ui_textbox_set_text(g_path_box, s_current_dir);
    ui_listbox_clear(g_listbox);

    load_directory(s_current_dir);

    for (int i = 0; i < s_num_entries; i++) {
        char line[200];
        if (s_entries[i].is_dir) {
            snprintf(line, sizeof(line), "[DIR]  %s", s_entries[i].name);
        } else {
            snprintf(line, sizeof(line), "[FILE] %s  (%u B)", s_entries[i].name, (unsigned int)s_entries[i].size);
        }
        ui_listbox_add_item(g_listbox, line);
    }

    char st[128];
    snprintf(st, sizeof(st), "%d items in %s", s_num_entries, s_current_dir);
    ui_label_set_text(g_status_lbl, st);
    ui_window_invalidate(g_win);
}

static void on_btn_up(ui_widget_t* w, void* user_data) {
    (void)w; (void)user_data;
    if (strcmp(s_current_dir, "/") == 0) return;

    char* last_slash = strrchr(s_current_dir, '/');
    if (last_slash && last_slash != s_current_dir) {
        *last_slash = '\0';
    } else {
        strcpy(s_current_dir, "/");
    }
    refresh_directory(s_current_dir);
}

static void on_btn_home(ui_widget_t* w, void* user_data) {
    (void)w; (void)user_data;
    refresh_directory("/home");
}

static void on_btn_refresh(ui_widget_t* w, void* user_data) {
    (void)w; (void)user_data;
    refresh_directory(s_current_dir);
}

static void open_selected_item(void) {
    const char* sel = ui_listbox_get_selected(g_listbox);
    if (!sel || !sel[0]) return;

    int is_dir = (strncmp(sel, "[DIR]  ", 7) == 0);
    const char* name_start = is_dir ? sel + 7 : sel + 7;

    char target_name[128];
    strncpy(target_name, name_start, sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';

    if (!is_dir) {
        /* Strip size suffix: "file.txt  (123 B)" */
        char* sp = strstr(target_name, "  (");
        if (sp) *sp = '\0';
    }

    if (is_dir) {
        char new_path[MAX_PATH_LEN];
        if (strcmp(s_current_dir, "/") == 0) {
            snprintf(new_path, sizeof(new_path), "/%s", target_name);
        } else {
            snprintf(new_path, sizeof(new_path), "%s/%s", s_current_dir, target_name);
        }
        refresh_directory(new_path);
    } else {
        char st[160];
        snprintf(st, sizeof(st), "Selected file: %s", target_name);
        ui_label_set_text(g_status_lbl, st);
        ui_window_invalidate(g_win);
    }
}

static void on_btn_open(ui_widget_t* w, void* user_data) {
    (void)w; (void)user_data;
    open_selected_item();
}

static void on_btn_delete(ui_widget_t* w, void* user_data) {
    (void)w; (void)user_data;
    const char* sel = ui_listbox_get_selected(g_listbox);
    if (!sel || !sel[0]) return;

    int is_dir = (strncmp(sel, "[DIR]  ", 7) == 0);
    const char* name_start = is_dir ? sel + 7 : sel + 7;
    char target_name[128];
    strncpy(target_name, name_start, sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';

    if (!is_dir) {
        char* sp = strstr(target_name, "  (");
        if (sp) *sp = '\0';
    }

    char full_path[MAX_PATH_LEN];
    if (strcmp(s_current_dir, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "/%s", target_name);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", s_current_dir, target_name);
    }

    if (!is_dir) {
        if (unlink(full_path) == 0) {
            char st[160];
            snprintf(st, sizeof(st), "Deleted %s", target_name);
            ui_label_set_text(g_status_lbl, st);
            refresh_directory(s_current_dir);
        } else {
            char st[160];
            snprintf(st, sizeof(st), "Failed to delete %s", target_name);
            ui_label_set_text(g_status_lbl, st);
            ui_window_invalidate(g_win);
        }
    } else {
        if (rmdir(full_path) == 0) {
            refresh_directory(s_current_dir);
        } else {
            char st[160];
            snprintf(st, sizeof(st), "Directory not empty: %s", target_name);
            ui_label_set_text(g_status_lbl, st);
            ui_window_invalidate(g_win);
        }
    }
}

static void on_list_select(ui_widget_t* w, int new_idx, void* user_data) {
    (void)w; (void)user_data;
    if (new_idx >= 0 && new_idx < s_num_entries) {
        char st[160];
        snprintf(st, sizeof(st), "%s: %s (%u B)",
                 s_entries[new_idx].is_dir ? "Folder" : "File",
                 s_entries[new_idx].name, (unsigned int)s_entries[new_idx].size);
        ui_label_set_text(g_status_lbl, st);
        ui_window_invalidate(g_win);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[files] Starting MyOS File Manager...\n");
    fflush(stdout);

    g_app = ui_app_create();
    if (!g_app) {
        fprintf(stderr, "[files] Failed to initialize libui app\n");
        return 1;
    }

    int win_w = 580;
    int win_h = 420;
    g_win = ui_window_create(g_app, win_w, win_h, "File Manager");
    if (!g_win) {
        fprintf(stderr, "[files] Failed to create window\n");
        ui_app_destroy(g_app);
        return 1;
    }

    ui_widget_t* root = ui_window_get_root(g_win);

    /* 1. Navigation Bar */
    ui_widget_t* btn_up = ui_button_create(g_win, 10, 10, 50, 28, "Up", on_btn_up);
    ui_widget_add_child(root, btn_up);

    ui_widget_t* btn_home = ui_button_create(g_win, 65, 10, 55, 28, "Home", on_btn_home);
    ui_widget_add_child(root, btn_home);

    ui_widget_t* btn_refresh = ui_button_create(g_win, 125, 10, 65, 28, "Refresh", on_btn_refresh);
    ui_widget_add_child(root, btn_refresh);

    g_path_box = ui_textbox_create(g_win, 195, 10, 375, 28, "/", NULL);
    ui_widget_add_child(root, g_path_box);

    /* 2. File List Box */
    g_listbox = ui_listbox_create(g_win, 10, 48, 560, 310, on_list_select);
    ui_widget_add_child(root, g_listbox);

    /* 3. Action Buttons */
    ui_widget_t* btn_open = ui_button_create(g_win, 10, 368, 70, 28, "Open", on_btn_open);
    ui_widget_add_child(root, btn_open);

    ui_widget_t* btn_delete = ui_button_create(g_win, 85, 368, 70, 28, "Delete", on_btn_delete);
    ui_widget_add_child(root, btn_delete);

    /* 4. Status Bar */
    g_status_lbl = ui_label_create(g_win, 165, 372, 405, 20, "Ready");
    ui_label_set_color(g_status_lbl, 0xFF475569);
    ui_widget_add_child(root, g_status_lbl);

    /* Initial directory load */
    refresh_directory("/");

    ui_window_show(g_win);
    printf("[files] File Manager window running\n");
    fflush(stdout);

    int ret = ui_app_run(g_app);
    ui_app_destroy(g_app);
    printf("[files] Exited cleanly.\n");
    return ret;
}

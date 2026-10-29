/*
 * ui_a11y.c - Accessibility Tree & Action Engine for MyOS Widget Toolkit (libui)
 *
 * Exposes a structured accessibility tree for all libui widgets and executes
 * accessibility actions through normal widget callback paths without pixel faking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json.h>
#include "ui_internal.h"

void ui_widget_set_secret(ui_widget_t* w, int is_secret) {
    if (w) w->is_secret = is_secret ? 1 : 0;
}

uint32_t ui_widget_get_a11y_id(ui_widget_t* w) {
    return w ? w->a11y_id : 0;
}

static ui_widget_t* find_widget_recursive(ui_widget_t* root, uint32_t id) {
    if (!root) return NULL;
    if (root->a11y_id == id) return root;

    for (int i = 0; i < root->num_children; i++) {
        ui_widget_t* res = find_widget_recursive(root->children[i], id);
        if (res) return res;
    }
    return NULL;
}

ui_widget_t* ui_window_find_by_a11y_id(ui_window_t* win, uint32_t a11y_id) {
    if (!win || !win->root) return NULL;
    return find_widget_recursive(win->root, a11y_id);
}

static const char* get_widget_role(ui_widget_t* w) {
    if (!w) return "unknown";
    if (w == w->win->root) return "window";

    switch (w->type) {
        case UI_WIDGET_CONTAINER: return "container";
        case UI_WIDGET_LABEL:     return "label";
        case UI_WIDGET_BUTTON:    return "button";
        case UI_WIDGET_CHECKBOX:  return "checkbox";
        case UI_WIDGET_RADIO:     return "radio";
        case UI_WIDGET_PROGRESS:  return "progress";
        case UI_WIDGET_TEXTBOX:   return "textbox";
        case UI_WIDGET_TEXTAREA:  return "textarea";
        case UI_WIDGET_LISTBOX:   return "listbox";
        case UI_WIDGET_SCROLLBAR: return "scrollbar";
        case UI_WIDGET_TABS:      return "tab";
        case UI_WIDGET_MENUBAR:   return "menu";
        case UI_WIDGET_CUSTOM:    return "custom";
        default:                  return "widget";
    }
}

static void get_widget_name(ui_widget_t* w, char* buf, size_t cap) {
    if (!w || !buf || cap == 0) return;
    buf[0] = '\0';

    if (w == w->win->root) {
        snprintf(buf, cap, "%s", w->win->title);
        return;
    }

    switch (w->type) {
        case UI_WIDGET_LABEL:
            snprintf(buf, cap, "%s", w->label.text);
            break;
        case UI_WIDGET_BUTTON:
            snprintf(buf, cap, "%s", w->button.text);
            break;
        case UI_WIDGET_CHECKBOX:
            snprintf(buf, cap, "%s", w->checkbox.text);
            break;
        case UI_WIDGET_RADIO:
            snprintf(buf, cap, "%s", w->radio.text);
            break;
        case UI_WIDGET_TEXTBOX:
            snprintf(buf, cap, "%s", w->textbox.text[0] ? w->textbox.text : "textbox");
            break;
        case UI_WIDGET_TABS:
            if (w->tabs.num_tabs > 0 && w->tabs.active_tab < w->tabs.num_tabs) {
                snprintf(buf, cap, "%s", w->tabs.tab_names[w->tabs.active_tab]);
            } else {
                snprintf(buf, cap, "%s", "tabs");
            }
            break;
        case UI_WIDGET_LISTBOX:
            snprintf(buf, cap, "%s", "listbox");
            break;
        case UI_WIDGET_MENUBAR:
            snprintf(buf, cap, "%s", "menubar");
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static void get_widget_value(ui_widget_t* w, char* buf, size_t cap) {
    if (!w || !buf || cap == 0) return;
    buf[0] = '\0';

    if (w->is_secret) {
        snprintf(buf, cap, "%s", "********");
        return;
    }

    switch (w->type) {
        case UI_WIDGET_TEXTBOX:
            snprintf(buf, cap, "%s", w->textbox.text);
            break;
        case UI_WIDGET_TEXTAREA:
            if (w->textarea.buffer) {
                snprintf(buf, cap, "%s", w->textarea.buffer);
            }
            break;
        case UI_WIDGET_PROGRESS:
            snprintf(buf, cap, "%d", w->progress.cur_val);
            break;
        case UI_WIDGET_SCROLLBAR:
            snprintf(buf, cap, "%d", w->scrollbar.cur_val);
            break;
        case UI_WIDGET_CHECKBOX:
            snprintf(buf, cap, "%s", w->checkbox.checked ? "checked" : "unchecked");
            break;
        case UI_WIDGET_RADIO:
            snprintf(buf, cap, "%s", w->radio.selected ? "selected" : "unselected");
            break;
        case UI_WIDGET_LISTBOX:
            if (w->listbox.selected_idx >= 0 && w->listbox.selected_idx < w->listbox.num_items) {
                snprintf(buf, cap, "%s", w->listbox.items[w->listbox.selected_idx]);
            }
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static void compute_absolute_bounds(ui_widget_t* w, int* out_x, int* out_y) {
    int ax = w->x;
    int ay = w->y;
    ui_widget_t* p = w->parent;
    while (p) {
        ax += p->x;
        ay += p->y;
        p = p->parent;
    }
    *out_x = ax;
    *out_y = ay;
}

static void serialize_widget_json(ui_widget_t* w, json_buf_t* jb, uint32_t current_depth, uint32_t max_depth) {
    if (!w || jb->error) return;

    json_buf_obj_start(jb);

    json_buf_obj_key(jb, "id");
    json_buf_append_int(jb, (long long)w->a11y_id);

    json_buf_obj_key(jb, "role");
    json_buf_append_str(jb, get_widget_role(w));

    char name_buf[128];
    get_widget_name(w, name_buf, sizeof(name_buf));
    json_buf_obj_key(jb, "name");
    json_buf_append_str(jb, name_buf);

    char val_buf[256];
    get_widget_value(w, val_buf, sizeof(val_buf));
    json_buf_obj_key(jb, "value");
    json_buf_append_str(jb, val_buf);

    /* States */
    json_buf_obj_key(jb, "states");
    json_buf_arr_start(jb);
    int first_state = 1;
    if (w->flags & UI_STATE_ENABLED) {
        if (!first_state) json_buf_append_raw(jb, ",", 1);
        json_buf_append_str(jb, "enabled");
        first_state = 0;
    }
    if (w->win && w->win->focused == w) {
        if (!first_state) json_buf_append_raw(jb, ",", 1);
        json_buf_append_str(jb, "focused");
        first_state = 0;
    }
    if ((w->type == UI_WIDGET_CHECKBOX && w->checkbox.checked) ||
        (w->type == UI_WIDGET_RADIO && w->radio.selected)) {
        if (!first_state) json_buf_append_raw(jb, ",", 1);
        json_buf_append_str(jb, "checked");
        first_state = 0;
    }
    if (!(w->flags & UI_STATE_VISIBLE)) {
        if (!first_state) json_buf_append_raw(jb, ",", 1);
        json_buf_append_str(jb, "hidden");
        first_state = 0;
    }
    json_buf_arr_end(jb);

    /* Bounds [x, y, w, h] */
    int ax = 0, ay = 0;
    compute_absolute_bounds(w, &ax, &ay);
    json_buf_obj_key(jb, "bounds");
    json_buf_arr_start(jb);
    json_buf_append_int(jb, ax);
    json_buf_append_raw(jb, ",", 1);
    json_buf_append_int(jb, ay);
    json_buf_append_raw(jb, ",", 1);
    json_buf_append_int(jb, w->w);
    json_buf_append_raw(jb, ",", 1);
    json_buf_append_int(jb, w->h);
    json_buf_arr_end(jb);

    /* Children */
    json_buf_obj_key(jb, "children");
    json_buf_arr_start(jb);

    int first_child = 1;
    if (max_depth == 0 || current_depth < max_depth) {
        /* Standard child widgets */
        for (int i = 0; i < w->num_children; i++) {
            if (!first_child) json_buf_append_raw(jb, ",", 1);
            first_child = 0;
            serialize_widget_json(w->children[i], jb, current_depth + 1, max_depth);
        }

        /* Virtual children for listbox items */
        if (w->type == UI_WIDGET_LISTBOX) {
            for (int i = 0; i < w->listbox.num_items; i++) {
                if (!first_child) json_buf_append_raw(jb, ",", 1);
                first_child = 0;

                json_buf_obj_start(jb);
                json_buf_obj_key(jb, "id");
                json_buf_append_int(jb, ((long long)w->a11y_id * 1000) + i + 1);
                json_buf_obj_key(jb, "role");
                json_buf_append_str(jb, "listitem");
                json_buf_obj_key(jb, "name");
                json_buf_append_str(jb, w->listbox.items[i] ? w->listbox.items[i] : "");
                json_buf_obj_key(jb, "value");
                json_buf_append_str(jb, "");
                json_buf_obj_key(jb, "states");
                json_buf_arr_start(jb);
                json_buf_append_str(jb, "enabled");
                if (i == w->listbox.selected_idx) {
                    json_buf_append_raw(jb, ",", 1);
                    json_buf_append_str(jb, "selected");
                }
                json_buf_arr_end(jb);
                json_buf_obj_key(jb, "bounds");
                json_buf_arr_start(jb);
                json_buf_append_int(jb, ax);
                json_buf_append_raw(jb, ",", 1);
                json_buf_append_int(jb, ay + 2 + (i * 20));
                json_buf_append_raw(jb, ",", 1);
                json_buf_append_int(jb, w->w - 4);
                json_buf_append_raw(jb, ",", 1);
                json_buf_append_int(jb, 20);
                json_buf_arr_end(jb);
                json_buf_obj_key(jb, "children");
                json_buf_arr_start(jb);
                json_buf_arr_end(jb);
                json_buf_obj_end(jb);
            }
        }
    }
    json_buf_arr_end(jb);

    json_buf_obj_end(jb);
}

void ui_a11y_on_get_tree(win_t* win, uint32_t root_id, uint32_t max_depth, void* udata) {
    ui_window_t* ui_win = (ui_window_t*)udata;
    if (!ui_win || !ui_win->root) {
        win_send_a11y_tree(win, "{}", 0);
        return;
    }

    ui_widget_t* target = (root_id == 0) ? ui_win->root : find_widget_recursive(ui_win->root, root_id);
    if (!target) target = ui_win->root;

    char out_buf[3500];
    json_buf_t jb;
    json_buf_init(&jb, out_buf, sizeof(out_buf));

    serialize_widget_json(target, &jb, 0, max_depth);

    int truncated = (jb.error != 0) ? 1 : 0;
    win_send_a11y_tree(win, out_buf, truncated);
}

void ui_a11y_on_action(win_t* win, uint32_t node_id, const char* action, const char* args, void* udata) {
    ui_window_t* ui_win = (ui_window_t*)udata;
    if (!ui_win || !ui_win->root || !action) {
        win_send_a11y_result(win, 0, "Window or action null", "");
        return;
    }

    /* Check if target is a virtual listbox item */
    ui_widget_t* target = NULL;
    int listitem_idx = -1;

    if (node_id > 1000) {
        uint32_t parent_id = node_id / 1000;
        target = find_widget_recursive(ui_win->root, parent_id);
        if (target && target->type == UI_WIDGET_LISTBOX) {
            listitem_idx = (int)(node_id % 1000) - 1;
        }
    }

    if (!target) {
        target = find_widget_recursive(ui_win->root, node_id);
    }

    if (!target) {
        win_send_a11y_result(win, 0, "Node not found", "");
        return;
    }

    int ok = 1;
    char err_buf[64] = "";
    char new_state[128] = "";

    /* Execute action */
    if (strcmp(action, "click") == 0) {
        ui_widget_set_focus(target);
        if (listitem_idx >= 0 && target->type == UI_WIDGET_LISTBOX) {
            target->listbox.selected_idx = listitem_idx;
            if (target->on_change) target->on_change(target, listitem_idx, target->user_data);
            snprintf(new_state, sizeof(new_state), "selected=%d", listitem_idx);
        } else if (target->type == UI_WIDGET_BUTTON) {
            if (target->on_click) target->on_click(target, target->user_data);
            strncpy(new_state, "clicked", sizeof(new_state) - 1);
        } else if (target->type == UI_WIDGET_CHECKBOX) {
            target->checkbox.checked = !target->checkbox.checked;
            if (target->on_change) target->on_change(target, target->checkbox.checked, target->user_data);
            snprintf(new_state, sizeof(new_state), "checked=%d", target->checkbox.checked);
        } else if (target->type == UI_WIDGET_RADIO) {
            target->radio.selected = 1;
            if (target->on_change) target->on_change(target, 1, target->user_data);
            strncpy(new_state, "selected=1", sizeof(new_state) - 1);
        } else if (target->type == UI_WIDGET_TABS) {
            if (args && args[0]) {
                int tab_idx = atoi(args);
                if (tab_idx >= 0 && tab_idx < target->tabs.num_tabs) {
                    target->tabs.active_tab = tab_idx;
                    snprintf(new_state, sizeof(new_state), "active_tab=%d", tab_idx);
                }
            }
        } else {
            if (target->on_click) target->on_click(target, target->user_data);
            strncpy(new_state, "clicked", sizeof(new_state) - 1);
        }
    } else if (strcmp(action, "focus") == 0) {
        ui_widget_set_focus(target);
        strncpy(new_state, "focused=1", sizeof(new_state) - 1);
    } else if (strcmp(action, "toggle") == 0) {
        if (target->type == UI_WIDGET_CHECKBOX) {
            target->checkbox.checked = !target->checkbox.checked;
            if (target->on_change) target->on_change(target, target->checkbox.checked, target->user_data);
            snprintf(new_state, sizeof(new_state), "checked=%d", target->checkbox.checked);
        } else {
            ok = 0;
            strncpy(err_buf, "Widget does not support toggle", sizeof(err_buf) - 1);
        }
    } else if (strcmp(action, "set_text") == 0) {
        const char* text_val = args ? args : "";
        if (target->type == UI_WIDGET_TEXTBOX) {
            ui_textbox_set_text(target, text_val);
            if (target->on_text_change) target->on_text_change(target, text_val, target->user_data);
            snprintf(new_state, sizeof(new_state), "text=%s", target->is_secret ? "********" : text_val);
        } else if (target->type == UI_WIDGET_TEXTAREA) {
            ui_textarea_set_text(target, text_val);
            strncpy(new_state, "text_updated", sizeof(new_state) - 1);
        } else {
            ok = 0;
            strncpy(err_buf, "Widget does not support set_text", sizeof(err_buf) - 1);
        }
    } else if (strcmp(action, "select") == 0) {
        if (target->type == UI_WIDGET_LISTBOX) {
            int sel_idx = -1;
            if (listitem_idx >= 0) {
                sel_idx = listitem_idx;
            } else if (args && args[0]) {
                /* Try numeric parse */
                char* endptr = NULL;
                long parsed = strtol(args, &endptr, 10);
                if (endptr && *endptr == '\0') {
                    sel_idx = (int)parsed;
                } else {
                    /* Match by item text */
                    for (int i = 0; i < target->listbox.num_items; i++) {
                        if (target->listbox.items[i] && strcmp(target->listbox.items[i], args) == 0) {
                            sel_idx = i;
                            break;
                        }
                    }
                }
            }
            if (sel_idx >= 0 && sel_idx < target->listbox.num_items) {
                target->listbox.selected_idx = sel_idx;
                if (target->on_change) target->on_change(target, sel_idx, target->user_data);
                snprintf(new_state, sizeof(new_state), "selected=%d", sel_idx);
            } else {
                ok = 0;
                strncpy(err_buf, "Invalid item selection", sizeof(err_buf) - 1);
            }
        } else if (target->type == UI_WIDGET_RADIO) {
            target->radio.selected = 1;
            if (target->on_change) target->on_change(target, 1, target->user_data);
            strncpy(new_state, "selected=1", sizeof(new_state) - 1);
        } else if (target->type == UI_WIDGET_TABS) {
            int tab_idx = atoi(args ? args : "0");
            if (tab_idx >= 0 && tab_idx < target->tabs.num_tabs) {
                target->tabs.active_tab = tab_idx;
                snprintf(new_state, sizeof(new_state), "active_tab=%d", tab_idx);
            }
        } else {
            ok = 0;
            strncpy(err_buf, "Widget does not support select", sizeof(err_buf) - 1);
        }
    } else if (strcmp(action, "scroll") == 0) {
        int delta = atoi(args ? args : "1");
        if (target->type == UI_WIDGET_SCROLLBAR) {
            target->scrollbar.cur_val += delta;
            if (target->on_change) target->on_change(target, target->scrollbar.cur_val, target->user_data);
            snprintf(new_state, sizeof(new_state), "val=%d", target->scrollbar.cur_val);
        } else if (target->type == UI_WIDGET_LISTBOX) {
            target->listbox.scroll_top += delta;
            snprintf(new_state, sizeof(new_state), "scroll=%d", target->listbox.scroll_top);
        } else {
            ok = 0;
            strncpy(err_buf, "Widget does not support scroll", sizeof(err_buf) - 1);
        }
    } else {
        ok = 0;
        snprintf(err_buf, sizeof(err_buf), "Unknown action '%s'", action);
    }

    win_send_a11y_result(win, ok, err_buf, new_state);

    if (ok) {
        ui_window_invalidate(ui_win);
    }
}

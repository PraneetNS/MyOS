/*
 * agentpanel.c - MyOS Agent Desktop Dashboard (libui)
 *
 * Stage 22: The Agent Desktop
 * Provides:
 *  - Header with active agent count, total token spend, and killswitch button
 *  - Presets loaded from /etc/agents.d/ config files
 *  - Running agents list with live stats and kill button
 *  - Ask bar (Super+Space focused) to query modeld over Intent Bus
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <ui.h>
#include <agent.h>
#include <intent.h>
#include <json.h>

typedef struct {
    char name[32];
    char principal[32];
    char desc[64];
    char cmd[128];
    uint32_t tokens;
    char caps_str[256];
} preset_t;

#define MAX_PRESETS 8
static preset_t g_presets[MAX_PRESETS];
static int g_num_presets = 0;

static ui_widget_t* g_lbl_stats = NULL;
static ui_widget_t* g_btn_killswitch = NULL;
static ui_widget_t* g_lb_agents = NULL;
static ui_widget_t* g_tb_ask = NULL;
static ui_widget_t* g_ta_reply = NULL;
static ui_widget_t* g_lbl_index = NULL;
static ui_widget_t* g_btn_mode = NULL;
static int g_memory_mode = 0;           /* 0 = agents list, 1 = Memory tab */
static char g_mem_principal[32];        /* principal whose memory is shown */

static void refresh_agents_list(void);

static void load_presets(void) {
    /* Set built-in default presets first */
    g_num_presets = 3;
    strncpy(g_presets[0].name, "Organizer", sizeof(g_presets[0].name) - 1);
    strncpy(g_presets[0].principal, "organizer", sizeof(g_presets[0].principal) - 1);
    strncpy(g_presets[0].desc, "File organizer agent", sizeof(g_presets[0].desc) - 1);
    strncpy(g_presets[0].cmd, "/bin/files.elf", sizeof(g_presets[0].cmd) - 1);
    g_presets[0].tokens = 50000;
    strncpy(g_presets[0].caps_str, "CAP_WINDOW_ACT:files,CAP_BUS_CALL:fs.,CAP_BUS_CALL:model.,CAP_BUS_CALL:mem.,CAP_MEM_READ:priv,CAP_MEM_WRITE:priv", sizeof(g_presets[0].caps_str) - 1);

    strncpy(g_presets[1].name, "Explainer", sizeof(g_presets[1].name) - 1);
    strncpy(g_presets[1].principal, "explainer", sizeof(g_presets[1].principal) - 1);
    strncpy(g_presets[1].desc, "Screen A11Y observer", sizeof(g_presets[1].desc) - 1);
    strncpy(g_presets[1].cmd, "/bin/term.elf", sizeof(g_presets[1].cmd) - 1);
    g_presets[1].tokens = 50000;
    strncpy(g_presets[1].caps_str, "CAP_WINDOW_OBSERVE:*,CAP_BUS_CALL:model.", sizeof(g_presets[1].caps_str) - 1);

    strncpy(g_presets[2].name, "Terminal Helper", sizeof(g_presets[2].name) - 1);
    strncpy(g_presets[2].principal, "termhelp", sizeof(g_presets[2].principal) - 1);
    strncpy(g_presets[2].desc, "Terminal assistant", sizeof(g_presets[2].desc) - 1);
    strncpy(g_presets[2].cmd, "/bin/term.elf", sizeof(g_presets[2].cmd) - 1);
    g_presets[2].tokens = 50000;
    strncpy(g_presets[2].caps_str, "CAP_WINDOW_ACT:term,CAP_BUS_CALL:model.,CAP_BUS_CALL:mem.,CAP_MEM_READ:priv,CAP_MEM_WRITE:priv", sizeof(g_presets[2].caps_str) - 1);

    DIR* d = opendir("/etc/agents.d");
    if (!d) return;

    struct dirent* de;
    int loaded = 0;
    while ((de = readdir(d)) != NULL && loaded < MAX_PRESETS) {
        if (de->d_name[0] == '.') continue;
        int len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 5, ".conf") != 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/etc/agents.d/%s", de->d_name);
        FILE* fp = fopen(path, "r");
        if (!fp) continue;

        preset_t p;
        memset(&p, 0, sizeof(p));
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            while (line[0] && (line[strlen(line) - 1] == '\n' || line[strlen(line) - 1] == '\r')) {
                line[strlen(line) - 1] = '\0';
            }
            if (strncmp(line, "name=", 5) == 0) {
                strncpy(p.name, line + 5, sizeof(p.name) - 1);
            } else if (strncmp(line, "principal=", 10) == 0) {
                strncpy(p.principal, line + 10, sizeof(p.principal) - 1);
            } else if (strncmp(line, "desc=", 5) == 0) {
                strncpy(p.desc, line + 5, sizeof(p.desc) - 1);
            } else if (strncmp(line, "cmd=", 4) == 0) {
                strncpy(p.cmd, line + 4, sizeof(p.cmd) - 1);
            } else if (strncmp(line, "tokens=", 7) == 0) {
                p.tokens = (uint32_t)atoi(line + 7);
            } else if (strncmp(line, "caps=", 5) == 0) {
                strncpy(p.caps_str, line + 5, sizeof(p.caps_str) - 1);
            }
        }
        fclose(fp);

        if (p.name[0] && p.cmd[0]) {
            if (loaded == 0) g_num_presets = 0;
            g_presets[loaded++] = p;
            g_num_presets = loaded;
        }
    }
    closedir(d);
}

static void launch_preset_idx(int idx) {
    if (idx < 0 || idx >= g_num_presets) return;
    preset_t* p = &g_presets[idx];

    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    budget.max_tokens = p->tokens > 0 ? p->tokens : 50000;
    budget.max_cpu_ticks = 100000;
    budget.max_mem_pages = 2048;

    char cstr[256];
    strncpy(cstr, p->caps_str, sizeof(cstr) - 1);
    cstr[sizeof(cstr) - 1] = '\0';

    char* token = strtok(cstr, ",");
    while (token) {
        while (*token == ' ') token++;
        if (strncmp(token, "CAP_WINDOW_ACT:", 15) == 0) {
            caps.mask |= CAP_WINDOW_ACT;
            if (caps.num_window_act < MAX_PATH_RULES) {
                strncpy(caps.window_act[caps.num_window_act++].prefix, token + 15, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_WINDOW_OBSERVE:", 19) == 0) {
            caps.mask |= CAP_WINDOW_OBSERVE;
            if (caps.num_window_observe < MAX_PATH_RULES) {
                strncpy(caps.window_observe[caps.num_window_observe++].prefix, token + 19, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_BUS_CALL:", 13) == 0) {
            caps.mask |= CAP_BUS_CALL;
            if (caps.num_bus_call < MAX_PATH_RULES) {
                strncpy(caps.bus_call[caps.num_bus_call++].prefix, token + 13, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_MEM_READ:", 13) == 0) {
            caps.mask |= CAP_MEM_READ;
            if (caps.num_mem_read < MAX_PATH_RULES) {
                strncpy(caps.mem_read[caps.num_mem_read++].prefix, token + 13, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_MEM_WRITE:", 14) == 0) {
            caps.mask |= CAP_MEM_WRITE;
            if (caps.num_mem_write < MAX_PATH_RULES) {
                strncpy(caps.mem_write[caps.num_mem_write++].prefix, token + 14, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_FS_READ:", 12) == 0) {
            caps.mask |= CAP_FS_READ;
            if (caps.num_fs_read < MAX_PATH_RULES) {
                strncpy(caps.fs_read[caps.num_fs_read++].prefix, token + 12, MAX_PATH_PREFIX_LEN - 1);
            }
        } else if (strncmp(token, "CAP_BUS_PROVIDE:", 16) == 0) {
            caps.mask |= CAP_BUS_PROVIDE;
            if (caps.num_bus_provide < MAX_PATH_RULES) {
                strncpy(caps.bus_provide[caps.num_bus_provide++].prefix, token + 16, MAX_PATH_PREFIX_LEN - 1);
            }
        }
        token = strtok(NULL, ",");
    }

    if (p->principal[0]) strncpy(caps.principal, p->principal, sizeof(caps.principal) - 1);
    int aid = agent_create(p->name, &caps, &budget);
    if (aid < 0) {
        printf("[agentpanel] agent_create failed: %d\n", aid);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(aid) < 0) {
            perror("agent_attach");
            exit(1);
        }
        execl(p->cmd, p->cmd, NULL);
        exit(1);
    }

    refresh_agents_list();
}

static void on_preset0_click(ui_widget_t* w, void* data) { (void)w; (void)data; launch_preset_idx(0); }
static void on_preset1_click(ui_widget_t* w, void* data) { (void)w; (void)data; launch_preset_idx(1); }
static void on_preset2_click(ui_widget_t* w, void* data) { (void)w; (void)data; launch_preset_idx(2); }

static void refresh_index_status(void) {
    if (!g_lbl_index) return;
    char line[96] = "Index: not running";
    int fd = open("/var/index/status", O_RDONLY);
    if (fd >= 0) {
        char raw[80];
        int n = read(fd, raw, sizeof(raw) - 1);
        close(fd);
        if (n > 0) {
            raw[n] = '\0';
            char* nl = strchr(raw, '\n');
            if (nl) *nl = '\0';
            snprintf(line, sizeof(line), "Index: %s", raw);
        }
    }
    ui_label_set_text(g_lbl_index, line);
}

static int mem_admin(const char* op, int id, char* res, size_t cap) {
    int fd = intent_connect();
    if (fd < 0) return -1;
    char args[256];
    json_buf_t jb;
    json_buf_init(&jb, args, sizeof(args));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "op");
    json_buf_append_str(&jb, op);
    json_buf_obj_key(&jb, "principal");
    json_buf_append_str(&jb, g_mem_principal);
    json_buf_obj_key(&jb, "id");
    json_buf_append_int(&jb, id);
    json_buf_obj_end(&jb);
    int rc = intent_call(fd, "mem.admin", args, res, cap, 4000);
    intent_close(fd);
    return rc;
}

static void refresh_memory_list(void) {
    ui_listbox_clear(g_lb_agents);
    static char res[8192];
    if (mem_admin("list", 0, res, sizeof(res)) != 0) {
        ui_listbox_add_item(g_lb_agents, "(memd unavailable)");
        return;
    }
    static json_doc_t doc;
    if (json_parse(&doc, res, strlen(res)) != JSON_ERR_OK || !doc.root) return;
    const json_val_t* arr = json_obj_get_arr(doc.root, "entries");
    size_t n = arr ? json_arr_len(arr) : 0;
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Memory of '%s': %u entries", g_mem_principal, (unsigned)n);
    ui_listbox_add_item(g_lb_agents, hdr);
    for (size_t i = 0; i < n; i++) {
        const json_val_t* e = json_arr_get(arr, i);
        int id = 0;
        json_obj_get_int(e, "id", &id);
        const char* prov = json_obj_get_str(e, "provenance");
        const char* text = json_obj_get_str(e, "text");
        char item[128];
        snprintf(item, sizeof(item), "M%d [%s] %.90s", id, prov ? prov : "?", text ? text : "");
        ui_listbox_add_item(g_lb_agents, item);
    }
}

static void refresh_agents_list(void) {
    if (!g_lb_agents) return;
    refresh_index_status();
    if (g_memory_mode) { refresh_memory_list(); return; }
    ui_listbox_clear(g_lb_agents);

    int active_count = 0;
    uint32_t total_spend = 0;

    for (int id = 1; id <= MAX_AGENTS; id++) {
        agent_info_t info;
        if (agent_info(id, &info) == 0 && info.state != AGENT_STATE_UNUSED) {
            active_count++;
            total_spend += info.usage.tokens_used;

            const char* st = "RUNNING";
            if (info.state == AGENT_STATE_SUSPENDED) st = "SUSPENDED";
            else if (info.state == AGENT_STATE_KILLED) st = "KILLED";

            char item[128];
            snprintf(item, sizeof(item), "#%-2d %-15s [%s] Tokens:%u/%u Procs:%u",
                     info.id, info.name, st,
                     (unsigned int)info.usage.tokens_used,
                     (unsigned int)info.budget.max_tokens,
                     (unsigned int)info.usage.procs_count);
            ui_listbox_add_item(g_lb_agents, item);
        }
    }

    if (g_lbl_stats) {
        char sbuf[128];
        snprintf(sbuf, sizeof(sbuf), "Active: %d | Total Spend: %u tokens",
                 active_count, (unsigned int)total_spend);
        ui_label_set_text(g_lbl_stats, sbuf);
    }

    if (g_btn_killswitch) {
        int ks = agent_killswitch_state();
        if (ks == 1) {
            ui_label_set_text(g_btn_killswitch, "Killswitch: ACTIVE");
            ui_label_set_color(g_btn_killswitch, 0xFFDC2626);
        } else {
            ui_label_set_text(g_btn_killswitch, "Killswitch: OFF");
            ui_label_set_color(g_btn_killswitch, 0xFF16A34A);
        }
    }
}

static void on_killswitch_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    agent_killswitch_toggle();
    refresh_agents_list();
}

static void on_refresh_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    refresh_agents_list();
}

static void on_kill_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    const char* sel = ui_listbox_get_selected(g_lb_agents);
    if (g_memory_mode) {
        if (!sel || sel[0] != 'M') return;
        static char res[256];
        mem_admin("delete", atoi(sel + 1), res, sizeof(res));
        refresh_agents_list();
        return;
    }
    if (!sel || sel[0] != '#') return;

    int id = atoi(sel + 1);
    if (id > 0) {
        agent_kill(id);
        refresh_agents_list();
    }
}

static void on_trust_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    if (!g_memory_mode) return;
    const char* sel = ui_listbox_get_selected(g_lb_agents);
    if (!sel || sel[0] != 'M') return;
    static char res[256];
    mem_admin("mark-trusted", atoi(sel + 1), res, sizeof(res));
    refresh_agents_list();
}

static void on_mode_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    if (!g_memory_mode) {
        strcpy(g_mem_principal, "system");
        const char* sel = ui_listbox_get_selected(g_lb_agents);
        if (sel && sel[0] == '#') {
            agent_info_t info;
            if (agent_info(atoi(sel + 1), &info) == 0 && info.principal[0]) {
                strncpy(g_mem_principal, info.principal, sizeof(g_mem_principal) - 1);
                g_mem_principal[sizeof(g_mem_principal) - 1] = '\0';
            }
        }
        g_memory_mode = 1;
        ui_label_set_text(g_btn_mode, "Agents");
    } else {
        g_memory_mode = 0;
        ui_label_set_text(g_btn_mode, "Memory");
    }
    refresh_agents_list();
}

static void on_send_click(ui_widget_t* w, void* data) {
    (void)w; (void)data;
    if (!g_tb_ask || !g_ta_reply) return;
    const char* prompt = ui_textbox_get_text(g_tb_ask);
    if (!prompt || !prompt[0]) return;

    ui_textarea_set_text(g_ta_reply, "Querying Intent Bus (model.complete)...\n");

    int ifd = intent_connect();
    if (ifd < 0) {
        ui_textarea_set_text(g_ta_reply, "Error: Could not connect to Intent Bus (/run/intent.sock).\nIs intentd running?");
        return;
    }

    /* Escape quotes in prompt */
    char clean_prompt[256];
    int pidx = 0;
    for (int i = 0; prompt[i] && pidx < (int)sizeof(clean_prompt) - 2; i++) {
        if (prompt[i] == '"' || prompt[i] == '\\') clean_prompt[pidx++] = '\\';
        clean_prompt[pidx++] = prompt[i];
    }
    clean_prompt[pidx] = '\0';

    char req[512];
    snprintf(req, sizeof(req), "{\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":128,\"temperature\":0.7}", clean_prompt);

    char res[4096];
    int rc = intent_call(ifd, "model.complete", req, res, sizeof(res), 15000);
    intent_close(ifd);

    if (rc < 0) {
        char err[128];
        snprintf(err, sizeof(err), "Error calling model.complete: code %d", rc);
        ui_textarea_set_text(g_ta_reply, err);
    } else {
        json_doc_t doc;
        if (json_parse(&doc, res, strlen(res)) == JSON_ERR_OK) {
            const char* text_val = json_obj_get_str(doc.root, "text");
            if (text_val && text_val[0]) {
                ui_textarea_set_text(g_ta_reply, text_val);
            } else {
                ui_textarea_set_text(g_ta_reply, res);
            }
        } else {
            ui_textarea_set_text(g_ta_reply, res);
        }
    }

    refresh_agents_list();
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    load_presets();

    ui_app_t* app = ui_app_create();
    if (!app) {
        fprintf(stderr, "agentpanel: cannot connect to display server\n");
        return 1;
    }

    ui_window_t* win = ui_window_create(app, 680, 530, "Agent Dashboard");
    if (!win) {
        ui_app_destroy(app);
        return 1;
    }

    ui_widget_t* root = ui_window_get_root(win);

    /* 1. Header Bar */
    ui_widget_t* lbl_title = ui_label_create(win, 20, 15, 200, 24, "MyOS Agent Panel");
    ui_label_set_color(lbl_title, 0xFF1E293B);
    ui_widget_add_child(root, lbl_title);

    g_lbl_stats = ui_label_create(win, 220, 15, 270, 24, "Active: 0 | Total Spend: 0 tokens");
    ui_label_set_color(g_lbl_stats, 0xFF475569);
    ui_widget_add_child(root, g_lbl_stats);

    g_btn_killswitch = ui_button_create(win, 500, 12, 160, 28, "Killswitch: OFF", on_killswitch_click);
    ui_widget_add_child(root, g_btn_killswitch);

    /* 2. Presets Card */
    ui_widget_t* lbl_pheader = ui_label_create(win, 20, 50, 200, 20, "Launch Preset Agent:");
    ui_widget_add_child(root, lbl_pheader);

    char p0_lbl[64]; snprintf(p0_lbl, sizeof(p0_lbl), "%s", g_num_presets > 0 ? g_presets[0].name : "Organizer");
    char p1_lbl[64]; snprintf(p1_lbl, sizeof(p1_lbl), "%s", g_num_presets > 1 ? g_presets[1].name : "Explainer");
    char p2_lbl[64]; snprintf(p2_lbl, sizeof(p2_lbl), "%s", g_num_presets > 2 ? g_presets[2].name : "Terminal Helper");

    ui_widget_t* b_p0 = ui_button_create(win, 20, 75, 200, 32, p0_lbl, on_preset0_click);
    ui_widget_add_child(root, b_p0);
    ui_widget_t* b_p1 = ui_button_create(win, 240, 75, 200, 32, p1_lbl, on_preset1_click);
    ui_widget_add_child(root, b_p1);
    ui_widget_t* b_p2 = ui_button_create(win, 460, 75, 200, 32, p2_lbl, on_preset2_click);
    ui_widget_add_child(root, b_p2);

    /* 3. Running Agents Table */
    ui_widget_t* lbl_rheader = ui_label_create(win, 20, 120, 170, 20, "Active Sandboxed Agents:");
    ui_widget_add_child(root, lbl_rheader);

    g_btn_mode = ui_button_create(win, 250, 115, 85, 26, "Memory", on_mode_click);
    ui_widget_add_child(root, g_btn_mode);
    ui_widget_t* btn_trust = ui_button_create(win, 345, 115, 95, 26, "Trust", on_trust_click);
    ui_widget_add_child(root, btn_trust);

    ui_widget_t* btn_refresh = ui_button_create(win, 450, 115, 95, 26, "Refresh", on_refresh_click);
    ui_widget_add_child(root, btn_refresh);
    ui_widget_t* btn_kill = ui_button_create(win, 555, 115, 105, 26, "Kill Agent", on_kill_click);
    ui_widget_add_child(root, btn_kill);

    g_lb_agents = ui_listbox_create(win, 20, 145, 640, 160, NULL);
    ui_widget_add_child(root, g_lb_agents);

    /* 4. Ask Bar (Super+Space) */
    ui_widget_t* lbl_ask = ui_label_create(win, 20, 320, 300, 20, "Ask Assistant (Super+Space):");
    ui_widget_add_child(root, lbl_ask);

    g_tb_ask = ui_textbox_create(win, 20, 345, 530, 32, "Ask modeld e.g. 'summarize open windows' or 'help me with bash'", NULL);
    ui_widget_add_child(root, g_tb_ask);

    ui_widget_t* btn_send = ui_button_create(win, 560, 345, 100, 32, "Send", on_send_click);
    ui_widget_add_child(root, btn_send);

    /* 5. Response TextArea */
    g_ta_reply = ui_textarea_create(win, 20, 385, 640, 125, "Ready for queries. Model responses from modeld will stream or display here.");
    ui_widget_add_child(root, g_ta_reply);

    g_lbl_index = ui_label_create(win, 20, 512, 640, 16, "Index: not running");
    ui_label_set_color(g_lbl_index, 0xFF475569);
    ui_widget_add_child(root, g_lbl_index);

    ui_window_show(win);
    ui_widget_set_focus(g_tb_ask);

    refresh_agents_list();

    int ret = ui_app_run(app);
    ui_app_destroy(app);
    return ret;
}

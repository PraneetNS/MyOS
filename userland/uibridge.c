/*
 * uibridge.c - Intent Bus UI Bridge Daemon for MyOS Agent Desktop
 *
 * Provides skills to agents over the Intent Bus:
 * - ui.list_windows
 * - ui.get_tree
 * - ui.act
 * - ui.launch
 * - ui.wait
 * - ui.focus_window
 *
 * Connects to wm as a privileged client and enforces dual capabilities:
 * caller agent sandbox + window manager allowlists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <agent.h>
#include <intent.h>
#include <json.h>
#include <win_protocol.h>

#define WM_SOCK_PATH "/run/wm.sock"

static int s_wm_fd = -1;

static int connect_to_wm(void) {
    if (s_wm_fd >= 0) {
        close(s_wm_fd);
        s_wm_fd = -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WIN_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        close(fd);
        return -1;
    }

    /* Send HELLO */
    struct win_msg_header hdr;
    hdr.len = sizeof(hdr) + sizeof(struct win_msg_hello);
    hdr.type = WIN_MSG_HELLO;
    hdr.flags = 0;
    hdr.window_id = 0;

    struct win_msg_hello hello;
    hello.version = WIN_PROTO_VERSION;
    hello.pid = (uint32_t)getpid();

    if (write(fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        write(fd, &hello, sizeof(hello)) != (int)sizeof(hello)) {
        close(fd);
        return -1;
    }

    /* Read HELLO_ACK */
    struct win_msg_header ack_hdr;
    struct win_msg_hello_ack ack_payload;
    if (read(fd, &ack_hdr, sizeof(ack_hdr)) != (int)sizeof(ack_hdr) ||
        read(fd, &ack_payload, sizeof(ack_payload)) != (int)sizeof(ack_payload)) {
        close(fd);
        return -1;
    }

    /* Send PRIVILEGED_REGISTER */
    hdr.len = sizeof(hdr) + sizeof(struct win_msg_privileged_register);
    hdr.type = WIN_MSG_PRIVILEGED_REGISTER;
    hdr.flags = 0;
    hdr.window_id = 0;

    struct win_msg_privileged_register preg;
    preg.role = 1; /* uibridge */

    if (write(fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        write(fd, &preg, sizeof(preg)) != (int)sizeof(preg)) {
        close(fd);
        return -1;
    }

    /* Read PRIVILEGED_ACK */
    struct win_msg_header pack_hdr;
    struct win_msg_privileged_ack pack_payload;
    if (read(fd, &pack_hdr, sizeof(pack_hdr)) != (int)sizeof(pack_hdr) ||
        read(fd, &pack_payload, sizeof(pack_payload)) != (int)sizeof(pack_payload) ||
        pack_payload.ok != 1) {
        close(fd);
        return -1;
    }

    s_wm_fd = fd;
    return 0;
}

static int ensure_wm_connected(void) {
    if (s_wm_fd >= 0) {
        struct pollfd pfd = { .fd = s_wm_fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            close(s_wm_fd);
            s_wm_fd = -1;
        }
    }
    if (s_wm_fd < 0) {
        return connect_to_wm();
    }
    return 0;
}

static int wm_exchange(uint16_t req_type, uint32_t win_id, const void* payload, uint32_t payload_len,
                       uint16_t* out_type, void* out_buf, size_t out_cap, uint32_t* out_len) {
    if (ensure_wm_connected() != 0) return -1;

    struct win_msg_header hdr;
    hdr.len = sizeof(hdr) + payload_len;
    hdr.type = req_type;
    hdr.flags = 0;
    hdr.window_id = win_id;

    if (write(s_wm_fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        close(s_wm_fd); s_wm_fd = -1; return -1;
    }
    if (payload_len > 0 && payload) {
        if (write(s_wm_fd, payload, payload_len) != (int)payload_len) {
            close(s_wm_fd); s_wm_fd = -1; return -1;
        }
    }

    /* Wait for response */
    struct pollfd pfd = { .fd = s_wm_fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 5000) <= 0) {
        close(s_wm_fd); s_wm_fd = -1; return -1;
    }

    struct win_msg_header resp_hdr;
    size_t rd = 0;
    while (rd < sizeof(resp_hdr)) {
        int n = read(s_wm_fd, (char*)&resp_hdr + rd, sizeof(resp_hdr) - rd);
        if (n <= 0) { close(s_wm_fd); s_wm_fd = -1; return -1; }
        rd += n;
    }

    *out_type = resp_hdr.type;
    uint32_t plen = resp_hdr.len > sizeof(resp_hdr) ? (resp_hdr.len - sizeof(resp_hdr)) : 0;
    uint32_t to_copy = plen < out_cap ? plen : out_cap;

    rd = 0;
    while (rd < to_copy) {
        int n = read(s_wm_fd, (char*)out_buf + rd, to_copy - rd);
        if (n <= 0) { close(s_wm_fd); s_wm_fd = -1; return -1; }
        rd += n;
    }

    /* Drain any remaining payload if out_cap was smaller */
    uint32_t rem = plen - to_copy;
    while (rem > 0) {
        char dummy[128];
        uint32_t chunk = rem > sizeof(dummy) ? sizeof(dummy) : rem;
        int n = read(s_wm_fd, dummy, chunk);
        if (n <= 0) break;
        rem -= n;
    }

    if (out_len) *out_len = to_copy;
    return 0;
}

/* Format JSON accessibility node tree into compact indented text */
static void format_node_to_text(const json_val_t* node, char* out, size_t* out_len, size_t max_cap, int depth) {
    if (!node || node->type != JSON_TYPE_OBJECT || *out_len >= max_cap - 64) return;

    for (int i = 0; i < depth && *out_len < max_cap - 64; i++) {
        out[(*out_len)++] = ' ';
        out[(*out_len)++] = ' ';
    }

    int id = 0;
    json_obj_get_int(node, "id", &id);
    const char* role = json_obj_get_str(node, "role");
    const char* name = json_obj_get_str(node, "name");
    const char* val = json_obj_get_str(node, "value");

    *out_len += snprintf(out + *out_len, max_cap - *out_len, "[%d] %s", id, role ? role : "widget");
    if (name && name[0]) {
        *out_len += snprintf(out + *out_len, max_cap - *out_len, " \"%s\"", name);
    }
    if (val && val[0]) {
        *out_len += snprintf(out + *out_len, max_cap - *out_len, " val=\"%s\"", val);
    }

    /* States */
    const json_val_t* states = json_obj_get_arr(node, "states");
    if (states) {
        size_t slen = json_arr_len(states);
        if (slen > 0) {
            *out_len += snprintf(out + *out_len, max_cap - *out_len, " states=[");
            for (size_t s = 0; s < slen && *out_len < max_cap - 64; s++) {
                if (s > 0) out[(*out_len)++] = ',';
                const json_val_t* st_item = json_arr_get(states, s);
                if (st_item && st_item->type == JSON_TYPE_STRING && st_item->u.str.ptr) {
                    *out_len += snprintf(out + *out_len, max_cap - *out_len, "%.*s",
                                        (int)st_item->u.str.len, st_item->u.str.ptr);
                }
            }
            *out_len += snprintf(out + *out_len, max_cap - *out_len, "]");
        }
    }

    /* Bounds */
    const json_val_t* bnds = json_obj_get_arr(node, "bounds");
    if (bnds && json_arr_len(bnds) >= 4) {
        const json_val_t* x_val = json_arr_get(bnds, 0);
        const json_val_t* y_val = json_arr_get(bnds, 1);
        const json_val_t* w_val = json_arr_get(bnds, 2);
        const json_val_t* h_val = json_arr_get(bnds, 3);
        int bx = x_val ? (int)x_val->u.num.int_val : 0;
        int by = y_val ? (int)y_val->u.num.int_val : 0;
        int bw = w_val ? (int)w_val->u.num.int_val : 0;
        int bh = h_val ? (int)h_val->u.num.int_val : 0;
        *out_len += snprintf(out + *out_len, max_cap - *out_len, " bounds=[%d,%d,%d,%d]", bx, by, bw, bh);
    }

    out[(*out_len)++] = '\n';
    out[*out_len] = '\0';

    /* Children */
    const json_val_t* children = json_obj_get_arr(node, "children");
    if (children) {
        size_t clen = json_arr_len(children);
        for (size_t c = 0; c < clen && *out_len < max_cap - 64; c++) {
            format_node_to_text(json_arr_get(children, c), out, out_len, max_cap, depth + 1);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.list_windows                                                     */
/* -------------------------------------------------------------------------- */
static int handle_ui_list_windows(const char* skill, const char* args_json,
                                 char* result_json, size_t result_cap,
                                 char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)args_json; (void)udata;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err_buf, err_cap, "Failed to open wm socket");
        return -1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WM_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        close(fd);
        snprintf(err_buf, err_cap, "Failed to connect to wm at %s", WM_SOCK_PATH);
        return -1;
    }

    if (write(fd, "LIST\n", 5) != 5) {
        close(fd);
        snprintf(err_buf, err_cap, "Failed to send LIST command to wm");
        return -1;
    }

    char resp[4096];
    int total_read = 0;
    while (total_read < (int)sizeof(resp) - 1) {
        int n = read(fd, resp + total_read, sizeof(resp) - 1 - total_read);
        if (n <= 0) break;
        total_read += n;
        if (strstr(resp, "OK\n")) break;
    }
    close(fd);
    resp[total_read] = '\0';

    /* Parse response into JSON array of windows */
    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "windows");
    json_buf_arr_start(&jb);

    char* line = strtok(resp, "\n");
    int first_entry = 1;
    while (line) {
        if (strncmp(line, "ID ", 3) == 0) {
            unsigned int wid = 0, agent = 0, w = 0, h = 0, foc = 0;
            int pid = 0, x = 0, y = 0;
            char title[128] = "";
            char app[64] = "";
            char state[32] = "";

            /* Format: ID %u PID %d TITLE "%s" APP "%s" AGENT %u X %d Y %d W %u H %u STATE %s FOCUS %d */
            char* p = line;
            if (sscanf(p, "ID %u PID %d TITLE \"%127[^\"]\" APP \"%63[^\"]\" AGENT %u X %d Y %d W %u H %u STATE %31s FOCUS %u",
                       &wid, &pid, title, app, &agent, &x, &y, &w, &h, state, &foc) >= 7) {
                if (!first_entry) json_buf_append_raw(&jb, ",", 1);
                first_entry = 0;

                json_buf_obj_start(&jb);
                json_buf_obj_key(&jb, "id");
                json_buf_append_int(&jb, wid);
                json_buf_obj_key(&jb, "pid");
                json_buf_append_int(&jb, pid);
                json_buf_obj_key(&jb, "title");
                json_buf_append_str(&jb, title);
                json_buf_obj_key(&jb, "app");
                json_buf_append_str(&jb, app);
                json_buf_obj_key(&jb, "agent");
                json_buf_append_int(&jb, agent);
                json_buf_obj_key(&jb, "x");
                json_buf_append_int(&jb, x);
                json_buf_obj_key(&jb, "y");
                json_buf_append_int(&jb, y);
                json_buf_obj_key(&jb, "w");
                json_buf_append_int(&jb, w);
                json_buf_obj_key(&jb, "h");
                json_buf_append_int(&jb, h);
                json_buf_obj_key(&jb, "state");
                json_buf_append_str(&jb, state);
                json_buf_obj_key(&jb, "focused");
                json_buf_append_bool(&jb, foc ? true : false);
                json_buf_obj_end(&jb);
            }
        }
        line = strtok(NULL, "\n");
    }

    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Buffer overflow formatting window list");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.get_tree                                                         */
/* -------------------------------------------------------------------------- */
static int handle_ui_get_tree(const char* skill, const char* args_json,
                              char* result_json, size_t result_cap,
                              char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;

    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK) {
        snprintf(err_buf, err_cap, "Malformed args JSON");
        return -1;
    }

    int win_id = 0;
    if (json_obj_get_int(doc.root, "window", &win_id) != 0 || win_id <= 0) {
        snprintf(err_buf, err_cap, "Missing or invalid 'window' ID");
        return -1;
    }

    int root_node = 0;
    json_obj_get_int(doc.root, "root", &root_node);
    int max_depth = 0;
    json_obj_get_int(doc.root, "depth", &max_depth);

    int caller_agent = intent_get_caller_agent();

    struct win_msg_a11y_get_tree req;
    req.root_node_id = (uint32_t)root_node;
    req.max_depth = (uint32_t)max_depth;
    req.caller_agent_id = (uint32_t)caller_agent;

    uint16_t resp_type = 0;
    char raw_tree_json[4096];
    uint32_t resp_len = 0;

    if (wm_exchange(WIN_MSG_A11Y_GET_TREE, (uint32_t)win_id, &req, sizeof(req),
                    &resp_type, raw_tree_json, sizeof(raw_tree_json) - 1, &resp_len) != 0) {
        snprintf(err_buf, err_cap, "Failed communicating with window manager");
        return -1;
    }
    raw_tree_json[resp_len] = '\0';

    if (resp_type == WIN_MSG_A11Y_RESULT) {
        struct win_msg_a11y_result res;
        memcpy(&res, raw_tree_json, sizeof(res));
        snprintf(err_buf, err_cap, "%s", res.error[0] ? res.error : "Failed to retrieve tree");
        return -1;
    }

    if (resp_type != WIN_MSG_A11Y_TREE) {
        snprintf(err_buf, err_cap, "Unexpected response from wm (%d)", resp_type);
        return -1;
    }

    /* Convert JSON tree to compact indented text */
    json_doc_t tree_doc;
    if (json_parse(&tree_doc, raw_tree_json, resp_len) != JSON_ERR_OK) {
        snprintf(result_json, result_cap, "{\"tree\":%s}", raw_tree_json);
        return 0;
    }

    char text_buf[3500];
    size_t text_len = 0;
    text_buf[0] = '\0';
    format_node_to_text(tree_doc.root, text_buf, &text_len, sizeof(text_buf), 0);

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "tree");
    json_buf_append_str(&jb, text_buf);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.act                                                              */
/* -------------------------------------------------------------------------- */
static int handle_ui_act(const char* skill, const char* args_json,
                         char* result_json, size_t result_cap,
                         char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;

    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK) {
        snprintf(err_buf, err_cap, "Malformed args JSON");
        return -1;
    }

    int win_id = 0;
    if (json_obj_get_int(doc.root, "window", &win_id) != 0 || win_id <= 0) {
        snprintf(err_buf, err_cap, "Missing or invalid 'window' ID");
        return -1;
    }

    int node_id = 0;
    if (json_obj_get_int(doc.root, "node", &node_id) != 0 || node_id < 0) {
        snprintf(err_buf, err_cap, "Missing or invalid 'node' ID");
        return -1;
    }

    const char* action = json_obj_get_str(doc.root, "action");
    if (!action) {
        snprintf(err_buf, err_cap, "Missing 'action' parameter");
        return -1;
    }

    const char* act_args = json_obj_get_str(doc.root, "args");
    if (!act_args) act_args = "";

    int caller_agent = intent_get_caller_agent();

    struct win_msg_a11y_action act;
    memset(&act, 0, sizeof(act));
    act.node_id = (uint32_t)node_id;
    act.caller_agent_id = (uint32_t)caller_agent;
    strncpy(act.action, action, sizeof(act.action) - 1);
    strncpy(act.args, act_args, sizeof(act.args) - 1);

    uint16_t resp_type = 0;
    struct win_msg_a11y_result res;
    memset(&res, 0, sizeof(res));
    uint32_t resp_len = 0;

    if (wm_exchange(WIN_MSG_A11Y_ACTION, (uint32_t)win_id, &act, sizeof(act),
                    &resp_type, &res, sizeof(res), &resp_len) != 0) {
        snprintf(err_buf, err_cap, "Failed communicating action to window manager");
        return -1;
    }

    if (!res.ok) {
        snprintf(err_buf, err_cap, "%s", res.error[0] ? res.error : "Action failed");
        return -1;
    }

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "ok");
    json_buf_append_bool(&jb, true);
    json_buf_obj_key(&jb, "new_state");
    json_buf_append_str(&jb, res.new_state);
    json_buf_obj_end(&jb);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.launch                                                           */
/* -------------------------------------------------------------------------- */
static int handle_ui_launch(const char* skill, const char* args_json,
                            char* result_json, size_t result_cap,
                            char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;

    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK) {
        snprintf(err_buf, err_cap, "Malformed args JSON");
        return -1;
    }

    const char* app = json_obj_get_str(doc.root, "app");
    if (!app) {
        snprintf(err_buf, err_cap, "Missing 'app' parameter");
        return -1;
    }

    /* Allowlisted apps */
    if (strcmp(app, "files") != 0 && strcmp(app, "term") != 0 &&
        strcmp(app, "uidemo") != 0 && strcmp(app, "agentpanel") != 0 &&
        strcmp(app, "paint") != 0 && strcmp(app, "view") != 0) {
        snprintf(err_buf, err_cap, "Application '%s' is not in GUI launch allowlist", app);
        return -1;
    }

    char bin_path[64];
    snprintf(bin_path, sizeof(bin_path), "/bin/%s.elf", app);
    if (access(bin_path, X_OK) != 0) {
        snprintf(bin_path, sizeof(bin_path), "/bin/%s", app);
        if (access(bin_path, X_OK) != 0) {
            snprintf(err_buf, err_cap, "Binary for '%s' not executable", app);
            return -1;
        }
    }

    int caller_agent = intent_get_caller_agent();

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err_buf, err_cap, "Failed to fork child process");
        return -1;
    }

    if (pid == 0) {
        if (caller_agent > 0) {
            if (agent_attach(caller_agent) != 0) {
                _exit(1);
            }
        }
        char* argv[] = { bin_path, NULL };
        execv(bin_path, argv);
        _exit(1);
    }

    usleep(150000); /* 150ms */

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "ok");
    json_buf_append_bool(&jb, true);
    json_buf_obj_key(&jb, "pid");
    json_buf_append_int(&jb, pid);
    json_buf_obj_end(&jb);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.wait                                                             */
/* -------------------------------------------------------------------------- */
static int handle_ui_wait(const char* skill, const char* args_json,
                          char* result_json, size_t result_cap,
                          char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;

    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK) {
        snprintf(err_buf, err_cap, "Malformed args JSON");
        return -1;
    }

    int win_id = 0;
    if (json_obj_get_int(doc.root, "window", &win_id) != 0 || win_id <= 0) {
        snprintf(err_buf, err_cap, "Missing 'window' ID");
        return -1;
    }

    int timeout_ms = 5000;
    json_obj_get_int(doc.root, "timeout", &timeout_ms);
    if (timeout_ms <= 0 || timeout_ms > 30000) timeout_ms = 5000;

    const json_val_t* until_val = json_obj_get(doc.root, "until");
    const char* target_role = NULL;
    const char* target_name = NULL;
    int target_id = -1;

    if (until_val && until_val->type == JSON_TYPE_OBJECT) {
        target_role = json_obj_get_str(until_val, "role");
        target_name = json_obj_get_str(until_val, "name");
        json_obj_get_int(until_val, "id", &target_id);
    }

    int caller_agent = intent_get_caller_agent();
    int elapsed = 0;
    bool found = false;

    while (elapsed < timeout_ms) {
        struct win_msg_a11y_get_tree req;
        req.root_node_id = 0;
        req.max_depth = 0;
        req.caller_agent_id = (uint32_t)caller_agent;

        uint16_t resp_type = 0;
        char raw_tree[4096];
        uint32_t resp_len = 0;

        if (wm_exchange(WIN_MSG_A11Y_GET_TREE, (uint32_t)win_id, &req, sizeof(req),
                        &resp_type, raw_tree, sizeof(raw_tree) - 1, &resp_len) == 0 &&
            resp_type == WIN_MSG_A11Y_TREE) {
            raw_tree[resp_len] = '\0';
            if (target_role && strstr(raw_tree, target_role)) found = true;
            if (target_name && strstr(raw_tree, target_name)) found = true;
            if (target_id > 0) {
                char id_str[32];
                snprintf(id_str, sizeof(id_str), "\"id\":%d", target_id);
                if (strstr(raw_tree, id_str)) found = true;
            }
            if (!target_role && !target_name && target_id <= 0) found = true;
            if (found) break;
        }

        usleep(100000); /* 100ms */
        elapsed += 100;
    }

    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "ok");
    json_buf_append_bool(&jb, true);
    json_buf_obj_key(&jb, "found");
    json_buf_append_bool(&jb, found);
    json_buf_obj_key(&jb, "elapsed_ms");
    json_buf_append_int(&jb, elapsed);
    json_buf_obj_end(&jb);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Skill: ui.focus_window                                                     */
/* -------------------------------------------------------------------------- */
static int handle_ui_focus_window(const char* skill, const char* args_json,
                                 char* result_json, size_t result_cap,
                                 char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;

    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK) {
        snprintf(err_buf, err_cap, "Malformed args JSON");
        return -1;
    }

    int win_id = 0;
    if (json_obj_get_int(doc.root, "window", &win_id) != 0 || win_id <= 0) {
        snprintf(err_buf, err_cap, "Missing 'window' ID");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err_buf, err_cap, "Failed to connect to wm");
        return -1;
    }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, WM_SOCK_PATH, sizeof(sun.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        close(fd);
        snprintf(err_buf, err_cap, "Cannot connect to wm control socket");
        return -1;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "FOCUS %d\n", win_id);
    write(fd, cmd, strlen(cmd));
    close(fd);

    snprintf(result_json, result_cap, "{\"ok\":true}");
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("uibridge: starting UI Bridge Daemon...\n");
    fflush(stdout);

    /* Try connecting to Window Manager display server */
    int wm_retries = 0;
    while (connect_to_wm() != 0) {
        wm_retries++;
        if (wm_retries % 10 == 0) {
            printf("uibridge: waiting for window manager at %s...\n", WIN_SOCK_PATH);
            fflush(stdout);
        }
        usleep(200000); /* 200ms */
    }
    printf("uibridge: registered privileged connection with wm\n");
    fflush(stdout);

    /* Connect to Intent Bus */
    int bus_fd = -1;
    while ((bus_fd = intent_connect()) < 0) {
        usleep(200000);
    }
    printf("uibridge: connected to intent bus\n");
    fflush(stdout);

    /* Register UI skills */
    intent_register_skill(bus_fd, "ui.list_windows", "List all active GUI windows and metadata",
                          "{\"type\":\"object\"}", NULL, false, handle_ui_list_windows, NULL);

    intent_register_skill(bus_fd, "ui.get_tree", "Get structured accessibility tree of a window",
                          "{\"type\":\"object\",\"properties\":{\"window\":{\"type\":\"integer\"}},\"required\":[\"window\"]}",
                          NULL, false, handle_ui_get_tree, NULL);

    intent_register_skill(bus_fd, "ui.act", "Perform accessibility action on a window widget",
                          "{\"type\":\"object\",\"properties\":{\"window\":{\"type\":\"integer\"},\"node\":{\"type\":\"integer\"},\"action\":{\"type\":\"string\"}},\"required\":[\"window\",\"node\",\"action\"]}",
                          NULL, false, handle_ui_act, NULL);

    intent_register_skill(bus_fd, "ui.launch", "Launch an allowlisted application inside agent sandbox",
                          "{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\"}},\"required\":[\"app\"]}",
                          NULL, false, handle_ui_launch, NULL);

    intent_register_skill(bus_fd, "ui.wait", "Wait for window node condition or timeout",
                          "{\"type\":\"object\",\"properties\":{\"window\":{\"type\":\"integer\"}},\"required\":[\"window\"]}",
                          NULL, false, handle_ui_wait, NULL);

    intent_register_skill(bus_fd, "ui.focus_window", "Bring a window to front and set focus",
                          "{\"type\":\"object\",\"properties\":{\"window\":{\"type\":\"integer\"}},\"required\":[\"window\"]}",
                          NULL, false, handle_ui_focus_window, NULL);

    printf("uibridge: all ui.* skills registered on Intent Bus; serving...\n");
    fflush(stdout);

    intent_serve(bus_fd);
    intent_close(bus_fd);
    if (s_wm_fd >= 0) close(s_wm_fd);
    return 0;
}

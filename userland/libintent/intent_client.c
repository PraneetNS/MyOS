#include "intent.h"
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_LOCAL_SKILLS 32

typedef struct {
    char name[64];
    intent_skill_handler_t handler;
    void* udata;
    bool active;
} local_skill_t;

static local_skill_t s_skills[MAX_LOCAL_SKILLS];
static int s_num_skills = 0;
static volatile int s_serve_running = 0;
static uint32_t s_next_call_id = 1;

static int s_current_caller_agent = 0;
static int s_current_caller_pid = 0;
static char s_current_caller_principal[32] = {0};

int intent_get_caller_agent(void) {
    return s_current_caller_agent;
}

int intent_get_caller_pid(void) {
    return s_current_caller_pid;
}

const char* intent_get_caller_principal(void) {
    return s_current_caller_principal;
}

int intent_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, INTENT_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void intent_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

static void extract_json_value(const char* p, char* out, size_t cap) {
    if (!p || !out || cap == 0) return;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '{' || *p == '[') {
        int depth = 0;
        const char* cur = p;
        while (*cur) {
            if (*cur == '"') {
                cur++;
                while (*cur && *cur != '"') {
                    if (*cur == '\\' && *(cur + 1)) cur++;
                    cur++;
                }
                if (*cur == '"') cur++;
                continue;
            }
            if (*cur == '{' || *cur == '[') depth++;
            else if (*cur == '}' || *cur == ']') {
                depth--;
                if (depth == 0) {
                    cur++;
                    break;
                }
            }
            cur++;
        }
        size_t len = (size_t)(cur - p);
        if (len >= cap) len = cap - 1;
        memcpy(out, p, len);
        out[len] = '\0';
    } else if (*p == '"') {
        const char* cur = p + 1;
        while (*cur && *cur != '"') {
            if (*cur == '\\' && *(cur + 1)) cur++;
            cur++;
        }
        if (*cur == '"') cur++;
        size_t len = (size_t)(cur - p);
        if (len >= cap) len = cap - 1;
        memcpy(out, p, len);
        out[len] = '\0';
    } else {
        const char* cur = p;
        while (*cur && *cur != ',' && *cur != '}' && *cur != ']' && *cur != ' ' && *cur != '\n') cur++;
        size_t len = (size_t)(cur - p);
        if (len >= cap) len = cap - 1;
        memcpy(out, p, len);
        out[len] = '\0';
    }
}

static int send_frame(int fd, uint16_t type, uint32_t call_id, const char* body, size_t body_len) {
    if (fd < 0) return -1;
    uint32_t total_len = sizeof(struct intent_header) + body_len;
    if (total_len > INTENT_MAX_MSG_LEN) return -1;

    struct intent_header hdr;
    hdr.len = total_len;
    hdr.type = type;
    hdr.flags = 0;
    hdr.call_id = call_id;

    size_t written = 0;
    while (written < sizeof(hdr)) {
        ssize_t n = write(fd, (const char*)&hdr + written, sizeof(hdr) - written);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        written += (size_t)n;
    }
    written = 0;
    while (written < body_len) {
        ssize_t n = write(fd, body + written, body_len - written);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int recv_frame(int fd, struct intent_header* out_hdr, char* body_buf, size_t body_cap, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int prc = poll(&pfd, 1, timeout_ms);
    if (prc <= 0) return (prc == 0) ? -ETIMEDOUT : -1;
    if (!(pfd.revents & POLLIN)) return -1;

    /* Read 12-byte header */
    size_t hdr_read = 0;
    while (hdr_read < sizeof(struct intent_header)) {
        ssize_t n = read(fd, (char*)out_hdr + hdr_read, sizeof(struct intent_header) - hdr_read);
        if (n <= 0) return -1;
        hdr_read += (size_t)n;
    }

    if (out_hdr->len < sizeof(struct intent_header) || out_hdr->len > INTENT_MAX_MSG_LEN) {
        return -1;
    }

    size_t payload_len = out_hdr->len - sizeof(struct intent_header);
    size_t to_read = payload_len;
    size_t to_store = (payload_len < body_cap) ? payload_len : (body_cap > 0 ? body_cap - 1 : 0);
    size_t body_read = 0;
    while (body_read < to_read) {
        char dummy[128];
        char* target = (body_read < to_store) ? (body_buf + body_read) : dummy;
        size_t chunk = (body_read < to_store) ? (to_store - body_read) : sizeof(dummy);
        if (chunk > to_read - body_read) chunk = to_read - body_read;
        ssize_t n = read(fd, target, chunk);
        if (n <= 0) return -1;
        body_read += (size_t)n;
    }
    if (body_cap > 0) body_buf[to_store] = '\0';
    if (payload_len >= body_cap) return -1;
    return 0;
}

int intent_register_skill(int fd, const char* name, const char* description,
                          const char* input_schema_json, const char* output_schema_json,
                          bool sensitive, intent_skill_handler_t handler, void* udata) {
    if (fd < 0 || !name || !handler) return -1;

    /* Record locally */
    int slot = -1;
    for (int i = 0; i < MAX_LOCAL_SKILLS; i++) {
        if (!s_skills[i].active || strcmp(s_skills[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    /* Build JSON message */
    char body[4096];
    json_buf_t jb;
    json_buf_init(&jb, body, sizeof(body));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "name");
    json_buf_append_str(&jb, name);
    json_buf_obj_key(&jb, "description");
    json_buf_append_str(&jb, description ? description : "");
    json_buf_obj_key(&jb, "sensitive");
    json_buf_append_bool(&jb, sensitive);

    if (input_schema_json && input_schema_json[0]) {
        json_buf_obj_key(&jb, "input_schema");
        json_buf_append_raw(&jb, input_schema_json, strlen(input_schema_json));
    }
    if (output_schema_json && output_schema_json[0]) {
        json_buf_obj_key(&jb, "output_schema");
        json_buf_append_raw(&jb, output_schema_json, strlen(output_schema_json));
    }
    json_buf_obj_end(&jb);

    if (jb.error) return -1;

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_REGISTER, cid, body, jb.len) < 0) {
        return -1;
    }

    struct intent_header rh;
    char resp[1024];
    if (recv_frame(fd, &rh, resp, sizeof(resp), INTENT_DEFAULT_TIMEOUT) < 0) {
        return -1;
    }

    static json_doc_t s_reg_doc;
    if (rh.type == INTENT_MSG_REGISTER_ACK) {
        if (json_parse(&s_reg_doc, resp, strlen(resp)) == JSON_ERR_OK) {
            bool ok = false;
            json_obj_get_bool(s_reg_doc.root, "ok", &ok);
            if (ok) {
                strncpy(s_skills[slot].name, name, sizeof(s_skills[slot].name) - 1);
                s_skills[slot].handler = handler;
                s_skills[slot].udata = udata;
                s_skills[slot].active = true;
                if (slot >= s_num_skills) s_num_skills = slot + 1;
                return 0;
            }
        }
    }
    return -1;
}

int intent_unregister_skill(int fd, const char* name) {
    if (fd < 0 || !name) return -1;

    char body[512];
    json_buf_t jb;
    json_buf_init(&jb, body, sizeof(body));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "name");
    json_buf_append_str(&jb, name);
    json_buf_obj_end(&jb);

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_UNREGISTER, cid, body, jb.len) < 0) {
        return -1;
    }

    struct intent_header rh;
    char resp[512];
    if (recv_frame(fd, &rh, resp, sizeof(resp), INTENT_DEFAULT_TIMEOUT) < 0) {
        return -1;
    }

    for (int i = 0; i < MAX_LOCAL_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].name, name) == 0) {
            s_skills[i].active = false;
            break;
        }
    }
    return 0;
}

int intent_serve(int fd) {
    if (fd < 0) return -1;
    s_serve_running = 1;

    static char body[INTENT_MAX_MSG_LEN];

    while (s_serve_running) {
        struct intent_header hdr;
        int rc = recv_frame(fd, &hdr, body, sizeof(body), 500);
        if (rc == -ETIMEDOUT) continue;
        if (rc < 0) return -1;

        if (hdr.type == INTENT_MSG_CALL_FORWARD) {
            static json_doc_t doc;
            if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) {
                continue;
            }

            int call_id = 0;
            json_obj_get_int(doc.root, "call_id", &call_id);
            int caller_agent = 0;
            json_obj_get_int(doc.root, "caller_agent", &caller_agent);
            int caller_pid = 0;
            json_obj_get_int(doc.root, "caller_pid", &caller_pid);
            s_current_caller_agent = caller_agent;
            s_current_caller_pid = caller_pid;
            const char* princ = json_obj_get_str(doc.root, "caller_principal");
            if (princ) {
                strncpy(s_current_caller_principal, princ, sizeof(s_current_caller_principal) - 1);
                s_current_caller_principal[sizeof(s_current_caller_principal) - 1] = '\0';
            } else {
                s_current_caller_principal[0] = '\0';
            }

            const char* skill_name = json_obj_get_str(doc.root, "skill");
            const json_val_t* args_val = json_obj_get(doc.root, "args");

            /* Find handler */
            local_skill_t* sk = NULL;
            if (skill_name) {
                for (int i = 0; i < s_num_skills; i++) {
                    if (s_skills[i].active && strcmp(s_skills[i].name, skill_name) == 0) {
                        sk = &s_skills[i];
                        break;
                    }
                }
            }

            static char result_buf[8192];
            static char err_buf[256];
            result_buf[0] = '\0';
            err_buf[0] = '\0';

            int handler_rc = -1;
            if (sk && sk->handler) {
                /* Format args as JSON string */
                static char args_str[4096];
                if (args_val && args_val->type == JSON_TYPE_STRING) {
                    strncpy(args_str, args_val->u.str.ptr, sizeof(args_str) - 1);
                    args_str[sizeof(args_str) - 1] = '\0';
                } else if (args_val) {
                    const char* raw_args = strstr(body, "\"args\":");
                    if (raw_args) {
                        raw_args += 7;
                        while (*raw_args == ' ') raw_args++;
                        extract_json_value(raw_args, args_str, sizeof(args_str));
                    } else {
                        strcpy(args_str, "{}");
                    }
                } else {
                    strcpy(args_str, "{}");
                }

                handler_rc = sk->handler(skill_name, args_str, result_buf, sizeof(result_buf),
                                         err_buf, sizeof(err_buf), sk->udata);
            } else {
                snprintf(err_buf, sizeof(err_buf), "Skill not found or inactive");
            }

            /* Send RESULT back to daemon */
            static char resp_body[INTENT_MAX_MSG_LEN];
            json_buf_t jb;
            json_buf_init(&jb, resp_body, sizeof(resp_body));
            json_buf_obj_start(&jb);
            json_buf_obj_key(&jb, "call_id");
            json_buf_append_int(&jb, call_id);
            json_buf_obj_key(&jb, "ok");
            json_buf_append_bool(&jb, handler_rc == 0);

            if (handler_rc == 0) {
                json_buf_obj_key(&jb, "result");
                if (result_buf[0] == '{' || result_buf[0] == '[' || result_buf[0] == '"') {
                    json_buf_append_raw(&jb, result_buf, strlen(result_buf));
                } else {
                    json_buf_append_str(&jb, result_buf);
                }
            } else {
                json_buf_obj_key(&jb, "error");
                json_buf_append_str(&jb, err_buf[0] ? err_buf : "Execution failed");
            }
            json_buf_obj_end(&jb);

            send_frame(fd, INTENT_MSG_RESULT, (uint32_t)call_id, resp_body, jb.len);
        }
    }
    return 0;
}

void intent_break_serve(void) {
    s_serve_running = 0;
}

int intent_call(int fd, const char* skill, const char* json_args,
                char* json_result, size_t result_cap, int timeout_ms) {
    if (fd < 0 || !skill) return -1;

    static char body[INTENT_MAX_MSG_LEN];
    json_buf_t jb;
    json_buf_init(&jb, body, sizeof(body));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "skill");
    json_buf_append_str(&jb, skill);
    if (timeout_ms > 0) {
        json_buf_obj_key(&jb, "timeout");
        json_buf_append_int(&jb, timeout_ms);
    }
    json_buf_obj_key(&jb, "args");
    if (json_args && (json_args[0] == '{' || json_args[0] == '[')) {
        json_buf_append_raw(&jb, json_args, strlen(json_args));
    } else if (json_args && json_args[0]) {
        json_buf_append_str(&jb, json_args);
    } else {
        json_buf_append_raw(&jb, "{}", 2);
    }
    json_buf_obj_end(&jb);

    if (jb.error) return -1;

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_CALL, cid, body, jb.len) < 0) {
        return -1;
    }

    if (timeout_ms <= 0) timeout_ms = INTENT_DEFAULT_TIMEOUT;

    struct intent_header rh;
    static char resp[INTENT_MAX_MSG_LEN];
    int rc = recv_frame(fd, &rh, resp, sizeof(resp), timeout_ms);
    if (rc < 0) {
        if (json_result && result_cap > 0) {
            snprintf(json_result, result_cap, "%s", (rc == -ETIMEDOUT) ? "Timed out" : "Call failed");
        }
        return -1;
    }

    static json_doc_t doc;
    if (rh.type == INTENT_MSG_RESULT) {
        if (json_parse(&doc, resp, strlen(resp)) == JSON_ERR_OK) {
            bool ok = false;
            json_obj_get_bool(doc.root, "ok", &ok);
            if (ok) {
                const char* res = strstr(resp, "\"result\":");
                if (res && json_result && result_cap > 0) {
                    res += 9;
                    while (*res == ' ') res++;
                    extract_json_value(res, json_result, result_cap);
                }
                return 0;
            } else {
                const char* err = json_obj_get_str(doc.root, "error");
                if (json_result && result_cap > 0) {
                    snprintf(json_result, result_cap, "%s", err ? err : "Call error");
                }
                return -1;
            }
        }
    } else if (rh.type == INTENT_MSG_ERROR) {
        if (json_parse(&doc, resp, strlen(resp)) == JSON_ERR_OK) {
            const char* err = json_obj_get_str(doc.root, "error");
            if (json_result && result_cap > 0) {
                snprintf(json_result, result_cap, "%s", err ? err : "Daemon error");
            }
        }
        return -1;
    }

    return -1;
}

int intent_list_skills(int fd, char* out_json, size_t cap) {
    if (fd < 0 || !out_json || cap == 0) return -1;

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_LIST, cid, "{}", 2) < 0) return -1;

    struct intent_header rh;
    int rc = recv_frame(fd, &rh, out_json, cap, INTENT_DEFAULT_TIMEOUT);
    if (rc < 0 || rh.type != INTENT_MSG_LIST_ACK) return -1;
    return 0;
}

int intent_list_approvals(int fd, char* out_json, size_t cap) {
    if (fd < 0 || !out_json || cap == 0) return -1;

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_LIST_APPROVALS, cid, "{}", 2) < 0) return -1;

    struct intent_header rh;
    int rc = recv_frame(fd, &rh, out_json, cap, INTENT_DEFAULT_TIMEOUT);
    if (rc < 0 || rh.type != INTENT_MSG_LIST_APPROVALS_ACK) return -1;
    return 0;
}

int intent_resolve_approval(int fd, uint32_t approval_id, bool approved, char* err_buf, size_t err_cap) {
    if (fd < 0) return -1;

    char body[256];
    json_buf_t jb;
    json_buf_init(&jb, body, sizeof(body));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "id");
    json_buf_append_int(&jb, approval_id);
    json_buf_obj_key(&jb, "approved");
    json_buf_append_bool(&jb, approved);
    json_buf_obj_end(&jb);

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_RESOLVE_APPROVAL, cid, body, jb.len) < 0) return -1;

    struct intent_header rh;
    char resp[512];
    int rc = recv_frame(fd, &rh, resp, sizeof(resp), INTENT_DEFAULT_TIMEOUT);
    if (rc < 0 || rh.type != INTENT_MSG_RESOLVE_APPROVAL_ACK) return -1;

    static json_doc_t doc;
    if (json_parse(&doc, resp, strlen(resp)) == JSON_ERR_OK) {
        bool ok = false;
        json_obj_get_bool(doc.root, "ok", &ok);
        if (!ok) {
            const char* err = json_obj_get_str(doc.root, "error");
            if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "%s", err ? err : "Resolve failed");
            return -1;
        }
    }
    return 0;
}

int intent_get_stats(int fd, char* out_json, size_t cap) {
    if (fd < 0 || !out_json || cap == 0) return -1;

    uint32_t cid = s_next_call_id++;
    if (send_frame(fd, INTENT_MSG_STATS, cid, "{}", 2) < 0) return -1;

    struct intent_header rh;
    int rc = recv_frame(fd, &rh, out_json, cap, INTENT_DEFAULT_TIMEOUT);
    if (rc < 0 || rh.type != INTENT_MSG_STATS_ACK) return -1;
    return 0;
}

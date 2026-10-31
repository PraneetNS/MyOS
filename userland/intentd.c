#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <agent.h>
#include <fcntl.h>
#include <signal.h>
#include <intent_protocol.h>
#include <approval_protocol.h>
#include <json.h>

#define MAX_CLIENTS    32
#define MAX_SKILLS     64
#define MAX_CALLS      64
#define MAX_APPROVALS  32

#define RATE_LIMIT_MAX_CALLS_PER_SEC 50
#define MAX_IN_FLIGHT_PER_CALLER     8

/* SHA-256 implementation */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)     ((x >> 2 | x << 30) ^ (x >> 13 | x << 19) ^ (x >> 22 | x << 10))
#define EP1(x)     ((x >> 6 | x << 26) ^ (x >> 11 | x << 21) ^ (x >> 25 | x << 7))
#define SIG0(x)    ((x >> 7 | x << 25) ^ (x >> 18 | x << 14) ^ (x >> 3))
#define SIG1(x)    ((x >> 17 | x << 15) ^ (x >> 19 | x << 13) ^ (x >> 10))

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(sha256_ctx_t* ctx, const uint8_t* data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (int i = 16; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t* ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            sha256_transform(ctx, ctx->buffer);
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8_t* hash) {
    uint8_t pad = 0x80;
    uint64_t total_bits = ctx->count * 8;
    sha256_update(ctx, &pad, 1);
    while ((ctx->count % 64) != 56) {
        pad = 0x00;
        sha256_update(ctx, &pad, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; --i) {
        len_bytes[i] = (uint8_t)(total_bits & 0xff);
        total_bits >>= 8;
    }
    sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i + (j * 4)] = (uint8_t)((ctx->state[j] >> (24 - i * 8)) & 0x000000ff);
        }
    }
}

static void hash_to_hex(const uint8_t* hash, char* hex_out, size_t hex_len) {
    static const char hexchars[] = "0123456789abcdef";
    size_t count = (hex_len - 1) / 2;
    if (count > 32) count = 32;
    for (size_t i = 0; i < count; i++) {
        hex_out[i * 2] = hexchars[(hash[i] >> 4) & 0xf];
        hex_out[i * 2 + 1] = hexchars[hash[i] & 0xf];
    }
    hex_out[count * 2] = '\0';
}

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Client descriptor */
typedef struct {
    int fd;
    pid_t pid;
    uint32_t agent_id;
    char principal[32];
    bool active;
    uint8_t rx_buf[INTENT_MAX_MSG_LEN];
    size_t rx_len;

    /* Rate limiting */
    uint64_t rate_sec_start;
    uint32_t calls_this_sec;

    /* In-flight calls */
    uint32_t in_flight;
} client_slot_t;

/* Skill descriptor */
typedef struct {
    char name[64];
    char description[128];
    char input_schema[2048];
    char output_schema[2048];
    bool sensitive;
    int provider_fd;
    pid_t provider_pid;
    uint32_t provider_agent_id;
    bool active;
} skill_slot_t;

/* In-flight call descriptor */
typedef struct {
    uint32_t call_id;
    uint32_t caller_req_id;
    int caller_fd;
    pid_t caller_pid;
    uint32_t caller_agent_id;
    char caller_principal[32];
    int provider_fd;
    uint32_t provider_agent_id;
    char skill[64];
    char args_hash_hex[17];
    uint64_t start_ms;
    uint64_t timeout_ms;
    bool pending_approval;
    uint32_t approval_id;
    char pending_body[INTENT_MAX_MSG_LEN];
    bool active;
} call_slot_t;

/* Pending approval descriptor */
typedef struct {
    uint32_t id;
    uint32_t call_id;
    uint32_t caller_agent;
    uint32_t provider_agent;
    char skill[64];
    uint64_t created_ms;
    bool active;
} approval_slot_t;

static client_slot_t s_clients[MAX_CLIENTS];
static skill_slot_t s_skills[MAX_SKILLS];
static call_slot_t s_calls[MAX_CALLS];
static approval_slot_t s_approvals[MAX_APPROVALS];

static uint32_t s_next_daemon_call_id = 1;
static uint32_t s_next_approval_id = 1;

static int s_approval_fd = -1;
static uint64_t s_last_approval_try = 0;

static void try_connect_approval_sock(void) {
    if (s_approval_fd >= 0) return;
    uint64_t now = get_now_ms();
    if (now - s_last_approval_try < 2000) return;
    s_last_approval_try = now;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, APPROVAL_SOCK_PATH, sizeof(sun.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sun, sizeof(sun)) == 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        s_approval_fd = fd;
    } else {
        close(fd);
    }
}

static void notify_wm_cancel_approval(uint32_t app_id) {
    if (s_approval_fd < 0) return;
    struct approval_msg_header hdr;
    hdr.len = sizeof(hdr) + sizeof(struct approval_msg_cancel);
    hdr.type = APPROVAL_MSG_CANCEL;
    hdr.flags = 0;
    hdr.call_id = 0;
    struct approval_msg_cancel can;
    can.approval_id = app_id;
    write(s_approval_fd, &hdr, sizeof(hdr));
    write(s_approval_fd, &can, sizeof(can));
}

/* Stats */
static uint64_t s_stats_total_calls = 0;
static uint64_t s_stats_succeeded = 0;
static uint64_t s_stats_denied = 0;
static uint64_t s_stats_timeouts = 0;

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

static bool skill_prefix_matches(const char* skill, const char* prefix) {
    if (!prefix || !skill) return false;
    if (strcmp(prefix, "*") == 0) return true;
    size_t plen = strlen(prefix);
    if (plen == 0) return true;
    if (strncmp(skill, prefix, plen) == 0) {
        if (prefix[plen - 1] == '.') return true;
        if (skill[plen] == '\0' || skill[plen] == '.') return true;
    }
    return false;
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

static bool check_agent_cap(uint32_t agent_id, uint32_t cap_flag, const char* skill_name) {
    if (agent_id == 0) return true; /* Unconfined non-agent process */

    agent_info_t info;
    if (agent_info((int)agent_id, &info) != 0) return false;

    if (!(info.caps.mask & cap_flag)) return false;

    if (cap_flag == CAP_BUS_CALL) {
        for (uint32_t i = 0; i < info.caps.num_bus_call && i < MAX_PATH_RULES; i++) {
            if (skill_prefix_matches(skill_name, info.caps.bus_call[i].prefix)) {
                return true;
            }
        }
        return false;
    }

    if (cap_flag == CAP_BUS_PROVIDE) {
        for (uint32_t i = 0; i < info.caps.num_bus_provide && i < MAX_PATH_RULES; i++) {
            if (skill_prefix_matches(skill_name, info.caps.bus_provide[i].prefix)) {
                return true;
            }
        }
        return false;
    }

    return true;
}

static void close_client(int slot) {
    if (slot < 0 || slot >= MAX_CLIENTS || !s_clients[slot].active) return;
    int fd = s_clients[slot].fd;

    /* Fail any in-flight calls waiting on this provider */
    for (int i = 0; i < MAX_CALLS; i++) {
        if (s_calls[i].active && s_calls[i].provider_fd == fd) {
            char resp[256];
            snprintf(resp, sizeof(resp), "{\"call_id\":%u,\"ok\":false,\"error\":\"Provider disconnected\"}",
                     (unsigned int)s_calls[i].caller_req_id);
            send_frame(s_calls[i].caller_fd, INTENT_MSG_RESULT, s_calls[i].caller_req_id, resp, strlen(resp));

            char detail[64];
            snprintf(detail, sizeof(detail), "c:%u p:%u %s rc:-104 h:%.8s",
                     (unsigned int)s_calls[i].caller_agent_id, (unsigned int)s_calls[i].provider_agent_id, s_calls[i].skill, s_calls[i].args_hash_hex);
            audit_append((int)s_calls[i].caller_agent_id, AUDIT_EVENT_BUS_CALL, detail);

            /* Decrement caller's in-flight count */
            for (int c = 0; c < MAX_CLIENTS; c++) {
                if (s_clients[c].active && s_clients[c].fd == s_calls[i].caller_fd && s_clients[c].in_flight > 0) {
                    s_clients[c].in_flight--;
                    break;
                }
            }
            s_calls[i].active = false;
        }
    }

    /* Cancel any in-flight calls from this caller */
    for (int i = 0; i < MAX_CALLS; i++) {
        if (s_calls[i].active && s_calls[i].caller_fd == fd) {
            s_calls[i].active = false;
        }
    }

    /* Clean up approvals belonging to this client */
    for (int i = 0; i < MAX_APPROVALS; i++) {
        if (s_approvals[i].active) {
            for (int c = 0; c < MAX_CALLS; c++) {
                if (s_calls[c].call_id == s_approvals[i].call_id &&
                    (s_calls[c].caller_fd == fd || s_calls[c].provider_fd == fd)) {
                    s_approvals[i].active = false;
                    break;
                }
            }
        }
    }

    /* Clean up all skills registered by this provider */
    for (int i = 0; i < MAX_SKILLS; i++) {
        if (s_skills[i].active && s_skills[i].provider_fd == fd) {
            s_skills[i].active = false;
        }
    }

    close(fd);
    s_clients[slot].active = false;
    s_clients[slot].fd = -1;
    s_clients[slot].rx_len = 0;
}

static void handle_msg_register(int cslot, const struct intent_header* hdr, const char* body) {
    client_slot_t* cli = &s_clients[cslot];
    static json_doc_t doc;
    if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) {
        send_frame(cli->fd, INTENT_MSG_REGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Invalid JSON\"}", 36);
        return;
    }

    const char* name = json_obj_get_str(doc.root, "name");
    const char* desc = json_obj_get_str(doc.root, "description");
    bool sensitive = false;
    json_obj_get_bool(doc.root, "sensitive", &sensitive);

    if (!name || strlen(name) == 0 || strlen(name) >= 64) {
        send_frame(cli->fd, INTENT_MSG_REGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Invalid skill name\"}", 42);
        return;
    }

    /* Check CAP_BUS_PROVIDE */
    if (!check_agent_cap(cli->agent_id, CAP_BUS_PROVIDE, name)) {
        char detail[64];
        snprintf(detail, sizeof(detail), "denied reg %s p:%u", name, (unsigned int)cli->agent_id);
        audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_DENIED, detail);
        s_stats_denied++;
        send_frame(cli->fd, INTENT_MSG_REGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Missing CAP_BUS_PROVIDE\"}", 47);
        return;
    }

    /* Find or allocate skill slot */
    int slot = -1;
    for (int i = 0; i < MAX_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_SKILLS; i++) {
            if (!s_skills[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        send_frame(cli->fd, INTENT_MSG_REGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Skill table full\"}", 39);
        return;
    }

    strncpy(s_skills[slot].name, name, sizeof(s_skills[slot].name) - 1);
    strncpy(s_skills[slot].description, desc ? desc : "", sizeof(s_skills[slot].description) - 1);
    s_skills[slot].sensitive = sensitive;
    s_skills[slot].provider_fd = cli->fd;
    s_skills[slot].provider_pid = cli->pid;
    s_skills[slot].provider_agent_id = cli->agent_id;

    /* Extract input_schema if provided */
    const json_val_t* schema_v = json_obj_get(doc.root, "input_schema");
    if (schema_v) {
        const char* p = strstr(body, "\"input_schema\":");
        if (p) {
            p += 15;
            while (*p == ' ') p++;
            strncpy(s_skills[slot].input_schema, p, sizeof(s_skills[slot].input_schema) - 1);
        }
    } else {
        s_skills[slot].input_schema[0] = '\0';
    }

    s_skills[slot].active = true;

    /* Log BUS_REGISTER to audit log */
    char detail[64];
    snprintf(detail, sizeof(detail), "reg %s p:%u sens:%d", name, (unsigned int)cli->agent_id, sensitive ? 1 : 0);
    audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_REGISTER, detail);

    send_frame(cli->fd, INTENT_MSG_REGISTER_ACK, hdr->call_id, "{\"ok\":true}", 11);
}

static void handle_msg_unregister(int cslot, const struct intent_header* hdr, const char* body) {
    client_slot_t* cli = &s_clients[cslot];
    static json_doc_t doc;
    if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) {
        send_frame(cli->fd, INTENT_MSG_UNREGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Invalid JSON\"}", 36);
        return;
    }

    const char* name = json_obj_get_str(doc.root, "name");
    if (!name) {
        send_frame(cli->fd, INTENT_MSG_UNREGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Missing name\"}", 35);
        return;
    }

    for (int i = 0; i < MAX_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].name, name) == 0) {
            if (s_skills[i].provider_fd == cli->fd || cli->agent_id == 0) {
                s_skills[i].active = false;
                send_frame(cli->fd, INTENT_MSG_UNREGISTER_ACK, hdr->call_id, "{\"ok\":true}", 11);
                return;
            }
        }
    }
    send_frame(cli->fd, INTENT_MSG_UNREGISTER_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Skill not found\"}", 38);
}

static void handle_msg_list(int cslot, const struct intent_header* hdr) {
    client_slot_t* cli = &s_clients[cslot];
    char out_buf[4096];
    json_buf_t jb;
    json_buf_init(&jb, out_buf, sizeof(out_buf));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "skills");
    json_buf_arr_start(&jb);

    int first_sk = 1;
    for (int i = 0; i < MAX_SKILLS; i++) {
        if (s_skills[i].active) {
            if (!first_sk) json_buf_append_raw(&jb, ",", 1);
            first_sk = 0;
            json_buf_obj_start(&jb);
            json_buf_obj_key(&jb, "name");
            json_buf_append_str(&jb, s_skills[i].name);
            json_buf_obj_key(&jb, "description");
            json_buf_append_str(&jb, s_skills[i].description);
            json_buf_obj_key(&jb, "provider_agent");
            json_buf_append_int(&jb, s_skills[i].provider_agent_id);
            json_buf_obj_key(&jb, "sensitive");
            json_buf_append_bool(&jb, s_skills[i].sensitive);
            json_buf_obj_end(&jb);
        }
    }
    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);

    send_frame(cli->fd, INTENT_MSG_LIST_ACK, hdr->call_id, out_buf, jb.len);
}

static void forward_call_to_provider(int call_slot) {
    call_slot_t* cs = &s_calls[call_slot];
    char fwd_body[INTENT_MAX_MSG_LEN];
    json_buf_t jb;
    json_buf_init(&jb, fwd_body, sizeof(fwd_body));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "call_id");
    json_buf_append_int(&jb, cs->call_id);
    json_buf_obj_key(&jb, "skill");
    json_buf_append_str(&jb, cs->skill);
    json_buf_obj_key(&jb, "caller_agent");
    json_buf_append_int(&jb, cs->caller_agent_id);
    json_buf_obj_key(&jb, "caller_pid");
    json_buf_append_int(&jb, cs->caller_pid);
    json_buf_obj_key(&jb, "caller_principal");
    json_buf_append_str(&jb, cs->caller_principal);
    json_buf_obj_key(&jb, "args");
    if (cs->pending_body[0]) {
        json_buf_append_raw(&jb, cs->pending_body, strlen(cs->pending_body));
    } else {
        json_buf_append_raw(&jb, "{}", 2);
    }
    json_buf_obj_end(&jb);

    send_frame(cs->provider_fd, INTENT_MSG_CALL_FORWARD, cs->call_id, fwd_body, jb.len);
}

static void handle_msg_call(int cslot, const struct intent_header* hdr, const char* body) {
    client_slot_t* cli = &s_clients[cslot];
    uint64_t now = get_now_ms();

    /* Rate limit check */
    if (now - cli->rate_sec_start >= 1000) {
        cli->rate_sec_start = now;
        cli->calls_this_sec = 0;
    }
    if (cli->calls_this_sec >= RATE_LIMIT_MAX_CALLS_PER_SEC) {
        s_stats_denied++;
        char detail[64];
        snprintf(detail, sizeof(detail), "denied c:%u rate limit exceeded", (unsigned int)cli->agent_id);
        audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_DENIED, detail);
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Rate limit exceeded\"}", 56);
        return;
    }
    cli->calls_this_sec++;

    /* In-flight check */
    if (cli->in_flight >= MAX_IN_FLIGHT_PER_CALLER) {
        s_stats_denied++;
        char detail[64];
        snprintf(detail, sizeof(detail), "denied c:%u in-flight limit reached", (unsigned int)cli->agent_id);
        audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_DENIED, detail);
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"In-flight cap exceeded\"}", 58);
        return;
    }

    static json_doc_t doc;
    if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) {
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Invalid JSON\"}", 48);
        return;
    }

    const char* skill_name = json_obj_get_str(doc.root, "skill");
    if (!skill_name) {
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Missing skill name\"}", 54);
        return;
    }

    /* Find skill */
    skill_slot_t* sk = NULL;
    for (int i = 0; i < MAX_SKILLS; i++) {
        if (s_skills[i].active && strcmp(s_skills[i].name, skill_name) == 0) {
            sk = &s_skills[i];
            break;
        }
    }
    if (!sk) {
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Skill not found\"}", 51);
        return;
    }

    /* Check CAP_BUS_CALL */
    if (!check_agent_cap(cli->agent_id, CAP_BUS_CALL, skill_name)) {
        s_stats_denied++;
        char detail[64];
        snprintf(detail, sizeof(detail), "denied %s c:%u p:%u cap_call", skill_name, (unsigned int)cli->agent_id, (unsigned int)sk->provider_agent_id);
        audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_DENIED, detail);
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Caller lacks CAP_BUS_CALL\"}", 62);
        return;
    }

    /* Validate args against input_schema */
    const json_val_t* args_val = json_obj_get(doc.root, "args");
    if (sk->input_schema[0]) {
        static json_doc_t sdoc;
        if (json_parse(&sdoc, sk->input_schema, strlen(sk->input_schema)) == JSON_ERR_OK && sdoc.root) {
            char err_buf[128];
            if (json_validate_schema(sdoc.root, args_val, err_buf, sizeof(err_buf)) != 0) {
                s_stats_denied++;
                char detail[64];
                snprintf(detail, sizeof(detail), "denied %s c:%u schema mismatch", skill_name, (unsigned int)cli->agent_id);
                audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_DENIED, detail);

                char resp[256];
                snprintf(resp, sizeof(resp), "{\"call_id\":0,\"ok\":false,\"error\":\"Schema validation error: %s\"}", err_buf);
                send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id, resp, strlen(resp));
                return;
            }
        }
    }

    /* Calculate SHA-256 of args */
    const char* raw_args = strstr(body, "\"args\":");
    static char args_body_str[INTENT_MAX_MSG_LEN];
    if (raw_args) {
        raw_args += 7;
        while (*raw_args == ' ') raw_args++;
        extract_json_value(raw_args, args_body_str, sizeof(args_body_str));
    } else {
        strcpy(args_body_str, "{}");
    }

    uint8_t hash[32];
    sha256_ctx_t hctx;
    sha256_init(&hctx);
    sha256_update(&hctx, (const uint8_t*)args_body_str, strlen(args_body_str));
    sha256_final(&hctx, hash);

    char hex_hash[33];
    hash_to_hex(hash, hex_hash, sizeof(hex_hash));

    /* Allocate in-flight call slot */
    int call_slot = -1;
    for (int i = 0; i < MAX_CALLS; i++) {
        if (!s_calls[i].active) {
            call_slot = i;
            break;
        }
    }
    if (call_slot < 0) {
        send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                   "{\"call_id\":0,\"ok\":false,\"error\":\"Bus daemon calls table full\"}", 63);
        return;
    }

    call_slot_t* cs = &s_calls[call_slot];
    cs->call_id = s_next_daemon_call_id++;
    cs->caller_req_id = hdr->call_id;
    cs->caller_fd = cli->fd;
    cs->caller_pid = cli->pid;
    cs->caller_agent_id = cli->agent_id;
    strncpy(cs->caller_principal, cli->principal, sizeof(cs->caller_principal) - 1);
    cs->caller_principal[sizeof(cs->caller_principal) - 1] = '\0';
    cs->provider_fd = sk->provider_fd;
    cs->provider_agent_id = sk->provider_agent_id;
    strncpy(cs->skill, skill_name, sizeof(cs->skill) - 1);
    memcpy(cs->args_hash_hex, hex_hash, 16);
    cs->args_hash_hex[16] = '\0';
    cs->start_ms = now;
    int req_timeout = 0;
    if (json_obj_get_int(doc.root, "timeout", &req_timeout) == 0 && req_timeout > 0) {
        if (req_timeout > 60000) req_timeout = 60000;
        cs->timeout_ms = (uint64_t)req_timeout;
    } else {
        cs->timeout_ms = INTENT_DEFAULT_TIMEOUT;
    }
    strncpy(cs->pending_body, args_body_str, sizeof(cs->pending_body) - 1);
    cs->active = true;
    cli->in_flight++;

    s_stats_total_calls++;

    if (sk->sensitive) {
        /* Create pending approval */
        int app_slot = -1;
        for (int i = 0; i < MAX_APPROVALS; i++) {
            if (!s_approvals[i].active) {
                app_slot = i;
                break;
            }
        }
        if (app_slot < 0) {
            cli->in_flight--;
            cs->active = false;
            send_frame(cli->fd, INTENT_MSG_RESULT, hdr->call_id,
                       "{\"call_id\":0,\"ok\":false,\"error\":\"Approvals table full\"}", 56);
            return;
        }

        approval_slot_t* as = &s_approvals[app_slot];
        as->id = s_next_approval_id++;
        as->call_id = cs->call_id;
        as->caller_agent = cli->agent_id;
        as->provider_agent = sk->provider_agent_id;
        strncpy(as->skill, skill_name, sizeof(as->skill) - 1);
        as->created_ms = now;
        as->active = true;

        cs->pending_approval = true;
        cs->approval_id = as->id;
        cs->timeout_ms = INTENT_APPROVAL_TIMEOUT;

        /* Write BUS_APPROVAL to audit log */
        char detail[64];
        snprintf(detail, sizeof(detail), "appr id:%u %s c:%u p:%u pending", (unsigned int)as->id, skill_name, (unsigned int)cli->agent_id, (unsigned int)sk->provider_agent_id);
        audit_append((int)cli->agent_id, AUDIT_EVENT_BUS_APPROVAL, detail);

        try_connect_approval_sock();
        if (s_approval_fd >= 0) {
            struct approval_msg_header a_hdr;
            a_hdr.len = sizeof(a_hdr) + sizeof(struct approval_msg_request);
            a_hdr.type = APPROVAL_MSG_REQUEST;
            a_hdr.flags = 0;
            a_hdr.call_id = 0;

            struct approval_msg_request a_req;
            memset(&a_req, 0, sizeof(a_req));
            a_req.approval_id = as->id;
            a_req.agent_id = cli->agent_id;

            char proc_path[64];
            snprintf(proc_path, sizeof(proc_path), "/proc/agents/%u/name", (unsigned int)cli->agent_id);
            int pfd = open(proc_path, O_RDONLY);
            if (pfd >= 0) {
                int rn = read(pfd, a_req.agent_name, sizeof(a_req.agent_name) - 1);
                if (rn > 0) {
                    while (rn > 0 && (a_req.agent_name[rn-1] == '\n' || a_req.agent_name[rn-1] == ' ' || a_req.agent_name[rn-1] == '\r')) {
                        a_req.agent_name[--rn] = '\0';
                    }
                }
                close(pfd);
            }
            if (!a_req.agent_name[0]) {
                snprintf(a_req.agent_name, sizeof(a_req.agent_name), "Agent #%u", (unsigned int)cli->agent_id);
            }

            strncpy(a_req.skill, skill_name, sizeof(a_req.skill) - 1);
            strncpy(a_req.args_summary, cs->pending_body, sizeof(a_req.args_summary) - 1);
            strncpy(a_req.args_hash, cs->args_hash_hex, sizeof(a_req.args_hash) - 1);
            a_req.sensitive = 1;
            a_req.timeout_ms = INTENT_APPROVAL_TIMEOUT;

            write(s_approval_fd, &a_hdr, sizeof(a_hdr));
            write(s_approval_fd, &a_req, sizeof(a_req));
        }
    } else {
        cs->pending_approval = false;
        forward_call_to_provider(call_slot);
    }
}

static void handle_msg_result(int cslot, const struct intent_header* hdr, const char* body) {
    client_slot_t* cli = &s_clients[cslot];
    static json_doc_t doc;
    if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) return;

    int call_id = 0;
    json_obj_get_int(doc.root, "call_id", &call_id);
    bool ok = false;
    json_obj_get_bool(doc.root, "ok", &ok);

    /* Find matching in-flight call */
    int call_slot = -1;
    for (int i = 0; i < MAX_CALLS; i++) {
        if (s_calls[i].active && s_calls[i].call_id == (uint32_t)call_id && s_calls[i].provider_fd == cli->fd) {
            call_slot = i;
            break;
        }
    }
    if (call_slot < 0) return;

    call_slot_t* cs = &s_calls[call_slot];

    /* Log BUS_CALL to audit log */
    int rc = ok ? 0 : -1;
    char detail[64];
    snprintf(detail, sizeof(detail), "c:%u p:%u %s rc:%d h:%.8s",
             (unsigned int)cs->caller_agent_id, (unsigned int)cs->provider_agent_id, cs->skill, rc, cs->args_hash_hex);
    audit_append((int)cs->caller_agent_id, AUDIT_EVENT_BUS_CALL, detail);

    if (ok) s_stats_succeeded++;
    else s_stats_denied++;

    /* Forward result to caller with caller's request call_id */
    send_frame(cs->caller_fd, INTENT_MSG_RESULT, cs->caller_req_id, body, strlen(body));

    /* Decrement caller's in-flight count */
    for (int c = 0; c < MAX_CLIENTS; c++) {
        if (s_clients[c].active && s_clients[c].fd == cs->caller_fd && s_clients[c].in_flight > 0) {
            s_clients[c].in_flight--;
            break;
        }
    }
    cs->active = false;
}

static void handle_msg_list_approvals(int cslot, const struct intent_header* hdr) {
    client_slot_t* cli = &s_clients[cslot];
    char out_buf[4096];
    json_buf_t jb;
    json_buf_init(&jb, out_buf, sizeof(out_buf));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "approvals");
    json_buf_arr_start(&jb);

    int first_app = 1;
    for (int i = 0; i < MAX_APPROVALS; i++) {
        if (s_approvals[i].active) {
            if (!first_app) json_buf_append_raw(&jb, ",", 1);
            first_app = 0;
            json_buf_obj_start(&jb);
            json_buf_obj_key(&jb, "id");
            json_buf_append_int(&jb, s_approvals[i].id);
            json_buf_obj_key(&jb, "skill");
            json_buf_append_str(&jb, s_approvals[i].skill);
            json_buf_obj_key(&jb, "caller_agent");
            json_buf_append_int(&jb, s_approvals[i].caller_agent);
            json_buf_obj_key(&jb, "provider_agent");
            json_buf_append_int(&jb, s_approvals[i].provider_agent);
            json_buf_obj_end(&jb);
        }
    }
    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);

    send_frame(cli->fd, INTENT_MSG_LIST_APPROVALS_ACK, hdr->call_id, out_buf, jb.len);
}

static bool resolve_approval_internal(uint32_t app_id, bool approved) {
    int app_slot = -1;
    for (int i = 0; i < MAX_APPROVALS; i++) {
        if (s_approvals[i].active && s_approvals[i].id == app_id) {
            app_slot = i;
            break;
        }
    }
    if (app_slot < 0) return false;

    approval_slot_t* as = &s_approvals[app_slot];

    /* Find corresponding in-flight call */
    int call_slot = -1;
    for (int i = 0; i < MAX_CALLS; i++) {
        if (s_calls[i].active && s_calls[i].call_id == as->call_id) {
            call_slot = i;
            break;
        }
    }

    if (call_slot >= 0) {
        call_slot_t* cs = &s_calls[call_slot];
        if (approved) {
            char detail[64];
            snprintf(detail, sizeof(detail), "appr id:%u %s c:%u p:%u granted", (unsigned int)as->id, as->skill, (unsigned int)as->caller_agent, (unsigned int)as->provider_agent);
            audit_append((int)as->caller_agent, AUDIT_EVENT_BUS_APPROVAL, detail);

            cs->pending_approval = false;
            cs->start_ms = get_now_ms();
            cs->timeout_ms = INTENT_DEFAULT_TIMEOUT;
            forward_call_to_provider(call_slot);
        } else {
            char detail[64];
            snprintf(detail, sizeof(detail), "appr id:%u %s c:%u p:%u denied", (unsigned int)as->id, as->skill, (unsigned int)as->caller_agent, (unsigned int)as->provider_agent);
            audit_append((int)as->caller_agent, AUDIT_EVENT_BUS_APPROVAL, detail);

            s_stats_denied++;
            send_frame(cs->caller_fd, INTENT_MSG_RESULT, cs->caller_req_id,
                       "{\"call_id\":0,\"ok\":false,\"error\":\"Call denied by approval authority\"}", 73);

            for (int c = 0; c < MAX_CLIENTS; c++) {
                if (s_clients[c].active && s_clients[c].fd == cs->caller_fd && s_clients[c].in_flight > 0) {
                    s_clients[c].in_flight--;
                    break;
                }
            }
            cs->active = false;
        }
    }

    notify_wm_cancel_approval(as->id);
    as->active = false;
    return true;
}

static void handle_msg_resolve_approval(int cslot, const struct intent_header* hdr, const char* body) {
    client_slot_t* cli = &s_clients[cslot];
    static json_doc_t doc;
    if (json_parse(&doc, body, strlen(body)) != JSON_ERR_OK) {
        send_frame(cli->fd, INTENT_MSG_RESOLVE_APPROVAL_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Invalid JSON\"}", 36);
        return;
    }

    int app_id = 0;
    json_obj_get_int(doc.root, "id", &app_id);
    bool approved = false;
    json_obj_get_bool(doc.root, "approved", &approved);

    if (!resolve_approval_internal((uint32_t)app_id, approved)) {
        send_frame(cli->fd, INTENT_MSG_RESOLVE_APPROVAL_ACK, hdr->call_id, "{\"ok\":false,\"error\":\"Approval ID not found\"}", 45);
        return;
    }

    send_frame(cli->fd, INTENT_MSG_RESOLVE_APPROVAL_ACK, hdr->call_id, "{\"ok\":true}", 11);
}

static void handle_msg_stats(int cslot, const struct intent_header* hdr) {
    client_slot_t* cli = &s_clients[cslot];
    uint32_t active_skills = 0;
    for (int i = 0; i < MAX_SKILLS; i++) if (s_skills[i].active) active_skills++;
    uint32_t active_clients = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_clients[i].active) active_clients++;
    uint32_t active_approvals = 0;
    for (int i = 0; i < MAX_APPROVALS; i++) if (s_approvals[i].active) active_approvals++;

    char out_buf[512];
    snprintf(out_buf, sizeof(out_buf),
             "{\"calls_total\":%llu,\"calls_succeeded\":%llu,\"calls_denied\":%llu,\"calls_timeouts\":%llu,\"active_skills\":%u,\"active_clients\":%u,\"active_approvals\":%u}",
             (unsigned long long)s_stats_total_calls,
             (unsigned long long)s_stats_succeeded,
             (unsigned long long)s_stats_denied,
             (unsigned long long)s_stats_timeouts,
             (unsigned int)active_skills, (unsigned int)active_clients, (unsigned int)active_approvals);

    send_frame(cli->fd, INTENT_MSG_STATS_ACK, hdr->call_id, out_buf, strlen(out_buf));
}

static void process_client_buffer(int slot) {
    client_slot_t* cli = &s_clients[slot];

    while (cli->rx_len >= sizeof(struct intent_header)) {
        struct intent_header hdr;
        memcpy(&hdr, cli->rx_buf, sizeof(hdr));

        if (hdr.len < sizeof(struct intent_header) || hdr.len > INTENT_MAX_MSG_LEN) {
            close_client(slot);
            return;
        }

        if (cli->rx_len < hdr.len) {
            /* Need more bytes */
            return;
        }

        /* Extract payload */
        size_t payload_len = hdr.len - sizeof(struct intent_header);
        static char body[INTENT_MAX_MSG_LEN];
        if (payload_len > 0) {
            memcpy(body, cli->rx_buf + sizeof(struct intent_header), payload_len);
        }
        body[payload_len] = '\0';

        /* Consume from rx_buf */
        size_t rem = cli->rx_len - hdr.len;
        if (rem > 0) {
            memmove(cli->rx_buf, cli->rx_buf + hdr.len, rem);
        }
        cli->rx_len = rem;

        /* Dispatch message */
        switch (hdr.type) {
            case INTENT_MSG_REGISTER:
                handle_msg_register(slot, &hdr, body);
                break;
            case INTENT_MSG_UNREGISTER:
                handle_msg_unregister(slot, &hdr, body);
                break;
            case INTENT_MSG_LIST:
                handle_msg_list(slot, &hdr);
                break;
            case INTENT_MSG_CALL:
                handle_msg_call(slot, &hdr, body);
                break;
            case INTENT_MSG_RESULT:
                handle_msg_result(slot, &hdr, body);
                break;
            case INTENT_MSG_LIST_APPROVALS:
                handle_msg_list_approvals(slot, &hdr);
                break;
            case INTENT_MSG_RESOLVE_APPROVAL:
                handle_msg_resolve_approval(slot, &hdr, body);
                break;
            case INTENT_MSG_STATS:
                handle_msg_stats(slot, &hdr);
                break;
            default:
                break;
        }

        if (!cli->active) return;
    }
}

static void check_timeouts(void) {
    uint64_t now = get_now_ms();

    for (int i = 0; i < MAX_CALLS; i++) {
        if (!s_calls[i].active) continue;
        call_slot_t* cs = &s_calls[i];

        if (now - cs->start_ms >= cs->timeout_ms) {
            s_stats_timeouts++;
            if (cs->pending_approval) {
                /* Auto-deny approval on timeout */
                char detail[64];
                snprintf(detail, sizeof(detail), "appr id:%u %s c:%u timeout-denied",
                         (unsigned int)cs->approval_id, cs->skill, (unsigned int)cs->caller_agent_id);
                audit_append((int)cs->caller_agent_id, AUDIT_EVENT_BUS_APPROVAL, detail);

                notify_wm_cancel_approval(cs->approval_id);

                for (int a = 0; a < MAX_APPROVALS; a++) {
                    if (s_approvals[a].active && s_approvals[a].id == cs->approval_id) {
                        s_approvals[a].active = false;
                        break;
                    }
                }

                send_frame(cs->caller_fd, INTENT_MSG_RESULT, cs->caller_req_id,
                           "{\"call_id\":0,\"ok\":false,\"error\":\"Approval timed out (auto-denied)\"}", 68);
            } else {
                /* Provider timed out */
                char detail[64];
                snprintf(detail, sizeof(detail), "c:%u p:%u %s rc:-110 h:%.8s",
                         (unsigned int)cs->caller_agent_id, (unsigned int)cs->provider_agent_id, cs->skill, cs->args_hash_hex);
                audit_append((int)cs->caller_agent_id, AUDIT_EVENT_BUS_CALL, detail);

                send_frame(cs->caller_fd, INTENT_MSG_RESULT, cs->caller_req_id,
                           "{\"call_id\":0,\"ok\":false,\"error\":\"Provider call timed out\"}", 57);
            }

            for (int c = 0; c < MAX_CLIENTS; c++) {
                if (s_clients[c].active && s_clients[c].fd == cs->caller_fd && s_clients[c].in_flight > 0) {
                    s_clients[c].in_flight--;
                    break;
                }
            }
            cs->active = false;
        }
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    signal(SIGPIPE, SIG_IGN);   /* prevent death on broken provider sockets */

    /* Register as privileged bus daemon */
    if (bus_daemon_register() != 0) {
        fprintf(stderr, "intentd: fatal: failed to register as bus daemon\n");
        return 1;
    }

    unlink(INTENT_SOCK_PATH);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("intentd: socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, INTENT_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("intentd: bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 16) < 0) {
        perror("intentd: listen");
        close(listen_fd);
        return 1;
    }

    printf("intentd: listening on %s (pid=%d)\n", INTENT_SOCK_PATH, getpid());

    while (1) {
        try_connect_approval_sock();

        struct pollfd fds[MAX_CLIENTS + 2];
        int num_fds = 0;

        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        num_fds++;

        int slot_map[MAX_CLIENTS + 2];
        slot_map[0] = -1;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].active) {
                fds[num_fds].fd = s_clients[i].fd;
                fds[num_fds].events = POLLIN;
                fds[num_fds].revents = 0;
                slot_map[num_fds] = i;
                num_fds++;
            }
        }

        int app_pfd_idx = -1;
        if (s_approval_fd >= 0) {
            app_pfd_idx = num_fds;
            fds[num_fds].fd = s_approval_fd;
            fds[num_fds].events = POLLIN;
            fds[num_fds].revents = 0;
            slot_map[num_fds] = -2;
            num_fds++;
        }

        int poll_rc = poll(fds, num_fds, 200); /* 200ms tick for timeouts */
        if (poll_rc < 0) {
            if (errno == EINTR) continue;
            perror("intentd: poll");
            break;
        }

        check_timeouts();

        if (poll_rc == 0) continue;

        /* Check approval socket events */
        if (app_pfd_idx >= 0 && (fds[app_pfd_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            struct approval_msg_header a_hdr;
            int n = read(s_approval_fd, &a_hdr, sizeof(a_hdr));
            if (n == (int)sizeof(a_hdr)) {
                if (a_hdr.type == APPROVAL_MSG_RESOLVE) {
                    struct approval_msg_resolve res;
                    if (read(s_approval_fd, &res, sizeof(res)) == (int)sizeof(res)) {
                        bool approved = (res.status == APPROVAL_STATUS_ALLOW || res.status == APPROVAL_STATUS_SESSION);
                        resolve_approval_internal(res.approval_id, approved);
                    }
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                close(s_approval_fd);
                s_approval_fd = -1;
            }
        }

        /* Accept incoming connection */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_un cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
            if (client_fd >= 0) {
                peerinfo_t pinfo;
                if (sock_peerinfo(client_fd, &pinfo) == 0) {
                    int slot = -1;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (!s_clients[i].active) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot >= 0) {
                        s_clients[slot].fd = client_fd;
                        s_clients[slot].pid = pinfo.pid;
                        s_clients[slot].agent_id = pinfo.agent_id;
                        strncpy(s_clients[slot].principal, pinfo.principal, sizeof(s_clients[slot].principal) - 1);
                        s_clients[slot].principal[sizeof(s_clients[slot].principal) - 1] = '\0';
                        s_clients[slot].active = true;
                        s_clients[slot].rx_len = 0;
                        s_clients[slot].rate_sec_start = get_now_ms();
                        s_clients[slot].calls_this_sec = 0;
                        s_clients[slot].in_flight = 0;
                    } else {
                        close(client_fd);
                    }
                } else {
                    close(client_fd);
                }
            }
        }

        /* Handle client I/O */
        for (int i = 1; i < num_fds; i++) {
            int slot = slot_map[i];
            if (slot < 0 || !s_clients[slot].active) continue;

            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close_client(slot);
                continue;
            }

            if (fds[i].revents & POLLIN) {
                client_slot_t* cli = &s_clients[slot];
                size_t space = INTENT_MAX_MSG_LEN - cli->rx_len;
                if (space == 0) {
                    /* Buffer full without valid framing, disconnect */
                    close_client(slot);
                    continue;
                }

                ssize_t n = read(cli->fd, cli->rx_buf + cli->rx_len, space);
                if (n <= 0) {
                    close_client(slot);
                    continue;
                }

                cli->rx_len += (size_t)n;
                process_client_buffer(slot);
            }
        }
    }

    close(listen_fd);
    unlink(INTENT_SOCK_PATH);
    return 0;
}

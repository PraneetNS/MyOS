#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <agent.h>
#include <intent.h>
#include <json.h>
#include <llm.h>

#define SERIAL_PORT_PATH "/dev/serial1"
#define MAX_SERIAL_FRAME 65536
#define BRIDGE_TIMEOUT_MS 15000

/* CRC32 lookup table */
static uint32_t s_crc32_table[256];
static int s_crc32_table_init = 0;

static void init_crc32_table(void) {
    if (s_crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        s_crc32_table[i] = c;
    }
    s_crc32_table_init = 1;
}

static uint32_t calc_crc32(const uint8_t* buf, size_t len) {
    init_crc32_table();
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        c = s_crc32_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}

/* SHA-256 for prompt hash */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} m_sha256_ctx_t;

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

static void m_sha256_transform(m_sha256_ctx_t* ctx, const uint8_t* data) {
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

static void m_sha256_init(m_sha256_ctx_t* ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void m_sha256_update(m_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            m_sha256_transform(ctx, ctx->buffer);
    }
}

static void m_sha256_final(m_sha256_ctx_t* ctx, uint8_t* hash) {
    uint8_t pad = 0x80;
    uint64_t total_bits = ctx->count * 8;
    m_sha256_update(ctx, &pad, 1);
    while ((ctx->count % 64) != 56) {
        pad = 0x00;
        m_sha256_update(ctx, &pad, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; --i) {
        len_bytes[i] = (uint8_t)(total_bits & 0xff);
        total_bits >>= 8;
    }
    m_sha256_update(ctx, len_bytes, 8);
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

/* Global state */
static llm_model_t s_local_model;
static bool s_local_loaded = false;
static int s_serial_fd = -1;
static uint32_t s_next_bridge_id = 1;

static int open_bridge_serial(void) {
    if (s_serial_fd >= 0) return s_serial_fd;
    s_serial_fd = open(SERIAL_PORT_PATH, O_RDWR | O_NONBLOCK);
    return s_serial_fd;
}

static void close_bridge_serial(void) {
    if (s_serial_fd >= 0) {
        close(s_serial_fd);
        s_serial_fd = -1;
    }
}

static int send_bridge_frame(int fd, const char* json_str, size_t len) {
    if (fd < 0 || !json_str || len == 0 || len > MAX_SERIAL_FRAME) return -1;
    uint32_t crc = calc_crc32((const uint8_t*)json_str, len);

    uint8_t hdr[8];
    hdr[0] = (uint8_t)((len >> 24) & 0xFF);
    hdr[1] = (uint8_t)((len >> 16) & 0xFF);
    hdr[2] = (uint8_t)((len >> 8) & 0xFF);
    hdr[3] = (uint8_t)(len & 0xFF);
    hdr[4] = (uint8_t)((crc >> 24) & 0xFF);
    hdr[5] = (uint8_t)((crc >> 16) & 0xFF);
    hdr[6] = (uint8_t)((crc >> 8) & 0xFF);
    hdr[7] = (uint8_t)(crc & 0xFF);

    /* Write 8-byte header */
    size_t written = 0;
    while (written < 8) {
        ssize_t n = write(fd, hdr + written, 8 - written);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) { usleep(1000); continue; }
            return -1;
        }
        written += (size_t)n;
    }

    /* Write payload */
    written = 0;
    while (written < len) {
        ssize_t n = write(fd, json_str + written, len - written);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) { usleep(1000); continue; }
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int recv_bridge_frame(int fd, char* out_buf, size_t cap, int timeout_ms) {
    if (fd < 0 || !out_buf || cap == 0) return -1;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    uint8_t rx[MAX_SERIAL_FRAME + 16];
    size_t rx_len = 0;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        pfd.revents = 0;
        int prc = poll(&pfd, 1, 50);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(fd, rx + rx_len, sizeof(rx) - rx_len);
            if (n > 0) {
                rx_len += (size_t)n;

                /* Scan rx for a valid frame */
                bool found = false;
                for (size_t offset = 0; offset + 8 <= rx_len; offset++) {
                    uint32_t flen = ((uint32_t)rx[offset] << 24) | ((uint32_t)rx[offset+1] << 16) |
                                   ((uint32_t)rx[offset+2] << 8) | (uint32_t)rx[offset+3];
                    uint32_t fcrc = ((uint32_t)rx[offset+4] << 24) | ((uint32_t)rx[offset+5] << 16) |
                                   ((uint32_t)rx[offset+6] << 8) | (uint32_t)rx[offset+7];

                    if (flen == 0 || flen > MAX_SERIAL_FRAME) continue;
                    if (rx_len < offset + 8 + flen) continue;

                    /* Check CRC */
                    uint32_t actual_crc = calc_crc32(rx + offset + 8, flen);
                    if (actual_crc == fcrc) {
                        /* Valid frame found! */
                        if (flen >= cap) flen = cap - 1;
                        memcpy(out_buf, rx + offset + 8, flen);
                        out_buf[flen] = '\0';
                        found = true;
                        return (int)flen;
                    }
                }
                (void)found;
            }
        }
        elapsed += 50;
    }
    return -ETIMEDOUT;
}

/* Local token callback accumulation */
typedef struct {
    char* buf;
    size_t cap;
    size_t len;
} local_accum_t;

static void local_token_cb(const char* piece, int token, void* udata) {
    (void)token;
    local_accum_t* acc = (local_accum_t*)udata;
    if (!acc || !piece) return;
    size_t plen = strlen(piece);
    if (acc->len + plen < acc->cap) {
        memcpy(acc->buf + acc->len, piece, plen);
        acc->len += plen;
        acc->buf[acc->len] = '\0';
    }
}

static void serialize_json_val(const json_val_t* v, json_buf_t* jb) {
    if (!v) { json_buf_append_null(jb); return; }
    switch (v->type) {
        case JSON_TYPE_NULL:
            json_buf_append_null(jb);
            break;
        case JSON_TYPE_BOOL:
            json_buf_append_bool(jb, v->u.boolean);
            break;
        case JSON_TYPE_NUMBER:
            if (v->u.num.is_int) {
                json_buf_append_int(jb, v->u.num.int_val);
            } else {
                char s[32];
                snprintf(s, sizeof(s), "%g", v->u.num.dbl_val);
                json_buf_append_raw(jb, s, strlen(s));
            }
            break;
        case JSON_TYPE_STRING:
            json_buf_append_str_n(jb, v->u.str.ptr, v->u.str.len);
            break;
        case JSON_TYPE_ARRAY: {
            json_buf_arr_start(jb);
            const json_val_t* curr = v->u.arr.head;
            bool first = true;
            while (curr) {
                if (!first) json_buf_append_raw(jb, ",", 1);
                first = false;
                serialize_json_val(curr, jb);
                curr = curr->next;
            }
            json_buf_arr_end(jb);
            break;
        }
        case JSON_TYPE_OBJECT: {
            json_buf_obj_start(jb);
            const json_val_t* curr = v->u.obj.head;
            while (curr) {
                if (curr->key) {
                    json_buf_obj_key(jb, curr->key);
                    serialize_json_val(curr, jb);
                }
                curr = curr->next;
            }
            json_buf_obj_end(jb);
            break;
        }
    }
}

/* Handler: model.status */
static int handle_model_status(const char* skill, const char* args_json,
                               char* result_json, size_t result_cap,
                               char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)args_json; (void)err_buf; (void)err_cap; (void)udata;

    int sfd = open_bridge_serial();
    bool host_ok = (sfd >= 0);

    snprintf(result_json, result_cap,
             "{\"backends\":{\"local\":%s,\"host\":%s},\"local_model\":\"stories-260k\",\"queue_depth\":0}",
             s_local_loaded ? "true" : "false",
             host_ok ? "true" : "false");
    return 0;
}

/* Handler: model.embed */
static int handle_model_embed(const char* skill, const char* args_json,
                              char* result_json, size_t result_cap,
                              char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    static json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Invalid JSON arguments");
        return -1;
    }

    const char* text = json_obj_get_str(doc.root, "text");
    if (!text) {
        snprintf(err_buf, err_cap, "Missing required argument 'text'");
        return -1;
    }

    /* 64-dimensional bag-of-words / hash embedding */
    float vec[64];
    memset(vec, 0, sizeof(vec));
    size_t tlen = strlen(text);

    for (size_t i = 0; i < tlen; i++) {
        uint32_t h = (uint8_t)text[i];
        if (i + 1 < tlen) h = (h * 31) ^ (uint8_t)text[i + 1];
        if (i + 2 < tlen) h = (h * 31) ^ (uint8_t)text[i + 2];
        int idx = (int)(h % 64);
        vec[idx] += 1.0f;
    }

    /* L2 normalize */
    float norm = 0.0f;
    for (int i = 0; i < 64; i++) norm += vec[i] * vec[i];
    norm = sqrtf(norm);
    if (norm > 1e-6f) {
        float inv = 1.0f / norm;
        for (int i = 0; i < 64; i++) vec[i] *= inv;
    }

    /* Serialize JSON vector */
    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "vector");
    json_buf_arr_start(&jb);
    for (int i = 0; i < 64; i++) {
        char numstr[32];
        snprintf(numstr, sizeof(numstr), "%.4f", vec[i]);
        if (i > 0) json_buf_append_raw(&jb, ",", 1);
        json_buf_append_raw(&jb, numstr, strlen(numstr));
    }
    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);

    if (jb.error) {
        snprintf(err_buf, err_cap, "Result buffer too small");
        return -1;
    }
    return 0;
}

/* Handler: model.complete */
static int handle_model_complete(const char* skill, const char* args_json,
                                 char* result_json, size_t result_cap,
                                 char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    int caller_agent = intent_get_caller_agent();

    /* Check remaining token budget */
    int tokens_left = agent_tokens_left(caller_agent);
    if (tokens_left <= 0) {
        snprintf(err_buf, err_cap, "Token budget exhausted");
        return -EDQUOT;
    }

    static json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Invalid JSON arguments");
        return -1;
    }

    const json_val_t* msgs_val = json_obj_get_arr(doc.root, "messages");
    if (!msgs_val) {
        snprintf(err_buf, err_cap, "Missing required array 'messages'");
        return -1;
    }

    int max_tokens = 256;
    json_obj_get_int(doc.root, "max_tokens", &max_tokens);
    if (max_tokens <= 0) max_tokens = 256;
    if (max_tokens > tokens_left) max_tokens = tokens_left;

    double temp_d = 0.7;
    const json_val_t* temp_v = json_obj_get(doc.root, "temperature");
    if (temp_v && temp_v->type == JSON_TYPE_NUMBER) {
        temp_d = temp_v->u.num.dbl_val;
    }
    float temperature = (float)temp_d;

    const char* backend_req = json_obj_get_str(doc.root, "backend");
    if (!backend_req || backend_req[0] == '\0') backend_req = "auto";

    /* Backend permissions check */
    if (caller_agent > 0) {
        agent_info_t info;
        if (agent_info(caller_agent, &info) == 0 && info.caps.model_backends != 0) {
            uint32_t mb = info.caps.model_backends;
            if (strcmp(backend_req, "host") == 0 && !(mb & MODEL_BACKEND_HOST)) {
                audit_append(caller_agent, AUDIT_EVENT_BUS_DENIED, "denied model backend:host");
                snprintf(err_buf, err_cap, "Agent lacks permission for host backend");
                return -EACCES;
            }
            if (strcmp(backend_req, "local") == 0 && !(mb & MODEL_BACKEND_LOCAL)) {
                audit_append(caller_agent, AUDIT_EVENT_BUS_DENIED, "denied model backend:local");
                snprintf(err_buf, err_cap, "Agent lacks permission for local backend");
                return -EACCES;
            }
        }
    }

    /* Hash the prompt for audit log */
    uint8_t phash[32];
    m_sha256_ctx_t sctx;
    m_sha256_init(&sctx);
    m_sha256_update(&sctx, (const uint8_t*)args_json, strlen(args_json));
    m_sha256_final(&sctx, phash);
    char prompt_hex[17];
    hash_to_hex(phash, prompt_hex, sizeof(prompt_hex));

    /* Decide backend */
    int sfd = open_bridge_serial();
    bool can_host = (sfd >= 0);
    bool can_local = s_local_loaded;

    char backend_selected[16];
    if (strcmp(backend_req, "host") == 0) {
        if (!can_host) {
            snprintf(err_buf, err_cap, "Host bridge unavailable");
            return -1;
        }
        strcpy(backend_selected, "host");
    } else if (strcmp(backend_req, "local") == 0) {
        if (!can_local) {
            snprintf(err_buf, err_cap, "Local model not loaded");
            return -1;
        }
        strcpy(backend_selected, "local");
    } else {
        /* "auto" backend resolution */
        if (can_host) {
            strcpy(backend_selected, "host");
        } else if (can_local) {
            strcpy(backend_selected, "local");
        } else {
            snprintf(err_buf, err_cap, "No model backend available");
            return -1;
        }
    }

    int in_tokens = 0;
    int out_tokens = 0;

    if (strcmp(backend_selected, "host") == 0) {
        /* Format request for host bridge */
        char bridge_req[8192];
        uint32_t req_id = s_next_bridge_id++;
        json_buf_t bjb;
        json_buf_init(&bjb, bridge_req, sizeof(bridge_req));
        json_buf_obj_start(&bjb);
        json_buf_obj_key(&bjb, "id");
        json_buf_append_int(&bjb, req_id);
        json_buf_obj_key(&bjb, "max_tokens");
        json_buf_append_int(&bjb, max_tokens);
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%.2f", (double)temperature);
        json_buf_obj_key(&bjb, "temperature");
        json_buf_append_raw(&bjb, tbuf, strlen(tbuf));
        json_buf_obj_key(&bjb, "messages");
        serialize_json_val(msgs_val, &bjb);
        const json_val_t* tools_val = json_obj_get_arr(doc.root, "tools");
        if (tools_val) {
            json_buf_obj_key(&bjb, "tools");
            serialize_json_val(tools_val, &bjb);
        }
        json_buf_obj_end(&bjb);

        if (send_bridge_frame(sfd, bridge_req, strlen(bridge_req)) != 0) {
            close_bridge_serial();
            if (strcmp(backend_req, "auto") == 0 && can_local) {
                /* Fallback to local */
                strcpy(backend_selected, "local");
            } else {
                snprintf(err_buf, err_cap, "Failed to write to host bridge");
                return -1;
            }
        } else {
            char bridge_resp[8192];
            int n = recv_bridge_frame(sfd, bridge_resp, sizeof(bridge_resp), BRIDGE_TIMEOUT_MS);
            if (n < 0) {
                close_bridge_serial();
                if (strcmp(backend_req, "auto") == 0 && can_local) {
                    /* Fallback to local */
                    strcpy(backend_selected, "local");
                } else {
                    snprintf(err_buf, err_cap, "Host bridge read timed out");
                    return -1;
                }
            } else {
                /* Host reply parsed */
                static json_doc_t hdoc;
                if (json_parse(&hdoc, bridge_resp, (size_t)n) == JSON_ERR_OK && hdoc.root) {
                    const json_val_t* usage_v = json_obj_get(hdoc.root, "usage");
                    if (usage_v) {
                        json_obj_get_int(usage_v, "in", &in_tokens);
                        json_obj_get_int(usage_v, "out", &out_tokens);
                    }
                    if (out_tokens <= 0) out_tokens = 10;
                    if (in_tokens <= 0) in_tokens = 20;

                    /* Copy host response into result_json with backend_used */
                    const char* ptext = json_obj_get_str(hdoc.root, "text");
                    const json_val_t* ptc = json_obj_get(hdoc.root, "tool_calls");

                    json_buf_t jb;
                    json_buf_init(&jb, result_json, result_cap);
                    json_buf_obj_start(&jb);
                    json_buf_obj_key(&jb, "text");
                    json_buf_append_str(&jb, ptext ? ptext : "");
                    json_buf_obj_key(&jb, "backend_used");
                    json_buf_append_str(&jb, "host");
                    json_buf_obj_key(&jb, "usage");
                    json_buf_obj_start(&jb);
                    json_buf_obj_key(&jb, "in");
                    json_buf_append_int(&jb, in_tokens);
                    json_buf_obj_key(&jb, "out");
                    json_buf_append_int(&jb, out_tokens);
                    json_buf_obj_key(&jb, "total");
                    json_buf_append_int(&jb, in_tokens + out_tokens);
                    json_buf_obj_end(&jb);

                    json_buf_obj_key(&jb, "tool_calls");
                    if (ptc) {
                        serialize_json_val(ptc, &jb);
                    } else {
                        json_buf_arr_start(&jb);
                        json_buf_arr_end(&jb);
                    }
                    json_buf_obj_end(&jb);

                    /* Charge actual tokens */
                    agent_charge_tokens(caller_agent, (uint32_t)(in_tokens + out_tokens));

                    /* Audit log */
                    char detail[64];
                    snprintf(detail, sizeof(detail), "model c:%u b:host in:%u out:%u h:%.8s",
                             (unsigned int)caller_agent, (unsigned int)in_tokens, (unsigned int)out_tokens, prompt_hex);
                    audit_append(caller_agent, AUDIT_EVENT_BUS_CALL, detail);
                    return 0;
                }
            }
        }
    }

    /* Local model path */
    if (strcmp(backend_selected, "local") == 0) {
        char prompt_flat[1024];
        prompt_flat[0] = '\0';
        size_t mcount = json_arr_len(msgs_val);
        for (size_t m = 0; m < mcount; m++) {
            const json_val_t* item = json_arr_get(msgs_val, m);
            if (!item) continue;
            const char* content = json_obj_get_str(item, "content");
            if (content) {
                if (prompt_flat[0]) strcat(prompt_flat, " ");
                strncat(prompt_flat, content, sizeof(prompt_flat) - strlen(prompt_flat) - 1);
            }
        }

        char out_text[4096];
        out_text[0] = '\0';
        local_accum_t acc = { out_text, sizeof(out_text), 0 };

        int generated = llm_generate(&s_local_model, prompt_flat, max_tokens, temperature, 42, local_token_cb, &acc);
        in_tokens = (int)strlen(prompt_flat) / 4 + 1;
        out_tokens = generated > 0 ? generated : 1;

        json_buf_t jb;
        json_buf_init(&jb, result_json, result_cap);
        json_buf_obj_start(&jb);
        json_buf_obj_key(&jb, "text");
        json_buf_append_str(&jb, out_text[0] ? out_text : "Local model inference completed.");
        json_buf_obj_key(&jb, "backend_used");
        json_buf_append_str(&jb, "local");
        json_buf_obj_key(&jb, "tool_calls");
        json_buf_arr_start(&jb);
        json_buf_arr_end(&jb);
        json_buf_obj_key(&jb, "usage");
        json_buf_obj_start(&jb);
        json_buf_obj_key(&jb, "in");
        json_buf_append_int(&jb, in_tokens);
        json_buf_obj_key(&jb, "out");
        json_buf_append_int(&jb, out_tokens);
        json_buf_obj_key(&jb, "total");
        json_buf_append_int(&jb, in_tokens + out_tokens);
        json_buf_obj_end(&jb);
        json_buf_obj_end(&jb);

        /* Charge tokens */
        agent_charge_tokens(caller_agent, (uint32_t)(in_tokens + out_tokens));

        /* Audit log */
        char detail[64];
        snprintf(detail, sizeof(detail), "model c:%u b:local in:%u out:%u h:%.8s",
                 (unsigned int)caller_agent, (unsigned int)in_tokens, (unsigned int)out_tokens, prompt_hex);
        audit_append(caller_agent, AUDIT_EVENT_BUS_CALL, detail);
        return 0;
    }

    snprintf(err_buf, err_cap, "Model execution failed");
    return -1;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    /* Register as privileged model daemon */
    if (model_daemon_register() != 0) {
        fprintf(stderr, "modeld: warning: failed to register as model daemon\n");
    }

    /* Load local model */
    if (llm_load(&s_local_model, "/share/stories.bin", "/share/tokenizer.bin") == 0) {
        s_local_loaded = true;
        printf("modeld: loaded local model (/share/stories.bin)\n");
    } else {
        s_local_loaded = false;
        printf("modeld: local model not loaded, running in host-only / stub mode\n");
    }

    /* Connect to Intent Bus */
    int bus_fd = -1;
    for (int retry = 0; retry < 50; retry++) {
        bus_fd = intent_connect();
        if (bus_fd >= 0) break;
        usleep(100000); /* 100ms */
    }

    if (bus_fd < 0) {
        fprintf(stderr, "modeld: fatal: cannot connect to intent bus\n");
        return 1;
    }

    /* Schemas */
    const char* complete_schema = "{\"type\":\"object\",\"properties\":{\"messages\":{\"type\":\"array\"}},\"required\":[\"messages\"]}";
    const char* embed_schema = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}";
    const char* status_schema = "{\"type\":\"object\"}";

    intent_register_skill(bus_fd, "model.complete", "Model chat/completion generation",
                          complete_schema, NULL, false, handle_model_complete, NULL);
    intent_register_skill(bus_fd, "model.embed", "Text feature vector embedding",
                          embed_schema, NULL, false, handle_model_embed, NULL);
    intent_register_skill(bus_fd, "model.status", "Model backends health and status",
                          status_schema, NULL, false, handle_model_status, NULL);

    printf("modeld: registered model skills on intent bus; serving...\n");
    fflush(stdout);

    intent_serve(bus_fd);
    intent_close(bus_fd);
    close_bridge_serial();
    if (s_local_loaded) llm_free(&s_local_model);
    return 0;
}

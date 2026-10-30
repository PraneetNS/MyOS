/*
 * memd - persistent agent memory daemon (Phase 5).
 *
 * Intent-bus provider for mem.* skills. Storage is /var/mem/<principal>/records.dat
 * (private namespace) or /var/mem/_shared_<name>/records.dat (shared namespaces,
 * ACLs in /etc/memory.conf). The file is an array of fixed 512-byte, CRC-protected
 * slots: a put is one slot write (torn write => bad CRC => slot ignored), forget
 * overwrites the slot with zeros (physical deletion). The search index (BM25 +
 * cosine over model.embed vectors) is derived and rebuilt from the slots on every
 * query, so there is no separate index file that could be corrupted.
 *
 * Identity: always the kernel-vouched caller (agent id + principal forwarded by
 * intentd from SYS_SOCK_PEERINFO), never anything in the message body.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <agent.h>
#include <intent.h>
#include <json.h>

#define SLOT_SIZE   512
#define SLOT_MAGIC  0x314D454Du
#define MAX_SLOTS   256
#define TEXT_MAX    383
#define VEC_DIM     64

enum { PROV_USER = 0, PROV_AGENT = 1, PROV_UNTRUSTED = 2 };
static const char* const s_prov_names[] = { "user", "agent", "untrusted" };

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t id;
    uint8_t  state;     /* 1 = live */
    uint8_t  prov;
    uint8_t  vec_ok;
    uint8_t  pad0;
    uint32_t ts;
    uint16_t tlen;
    uint16_t pad1;
    char     tags[32];
    int8_t   vec[VEC_DIM];
    char     text[TEXT_MAX + 1];
    uint8_t  reserved[8];
} slot_t;

typedef char slot_size_check[(sizeof(slot_t) == SLOT_SIZE) ? 1 : -1];

static slot_t s_slots[MAX_SLOTS];
static int    s_nslots;           /* slots physically present in file */
static int    s_call_fd = -1;     /* outbound bus connection for model.embed */

static uint32_t crc32_buf(const void* data, size_t n) {
    const uint8_t* p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (-(int32_t)(c & 1)));
    }
    return ~c;
}

static uint32_t slot_crc(const slot_t* s) {
    slot_t tmp = *s;
    tmp.crc = 0;
    return crc32_buf(&tmp, sizeof(tmp));
}

static bool slot_valid(const slot_t* s) {
    return s->magic == SLOT_MAGIC && s->state == 1 && s->crc == slot_crc(s) && s->tlen <= TEXT_MAX;
}

/* ---------- paths / namespaces ---------- */

static void sanitize(const char* in, char* out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        char c = in[i];
        out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
    }
    out[j] = '\0';
}

static void ensure_dir(const char* path) {
    mkdir(path, 0755);
}

/* dir/file for namespace ns of principal. Returns 0 or -1 if ns is invalid. */
static int ns_paths(const char* principal, const char* ns, char* dir, size_t dcap, char* file, size_t fcap) {
    char san[48];
    if (strcmp(ns, "priv") == 0) {
        sanitize(principal, san, sizeof(san));
        if (!san[0]) return -1;
        snprintf(dir, dcap, "/var/mem/%s", san);
    } else if (strncmp(ns, "shared:", 7) == 0 && ns[7]) {
        sanitize(ns + 7, san, sizeof(san));
        snprintf(dir, dcap, "/var/mem/_shared_%.40s", san);
    } else {
        return -1;
    }
    snprintf(file, fcap, "%s/records.dat", dir);
    return 0;
}

static void ensure_ns_dir(const char* dir) {
    ensure_dir("/var");
    ensure_dir("/var/mem");
    ensure_dir(dir);
}

/* ---------- /etc/memory.conf ACL ----------
 * Lines: shared:<name> read=a,b,* write=a,b
 */
static bool list_has(const char* list, const char* principal) {
    char buf[160];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* save = NULL;
    for (char* t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (strcmp(t, "*") == 0 || strcmp(t, principal) == 0) return true;
    }
    return false;
}

static bool shared_acl_ok(const char* ns, const char* principal, bool write) {
    FILE* fp = fopen("/etc/memory.conf", "r");
    if (!fp) return false;
    char line[256];
    bool ok = false;
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (line[0] == '#' || strncmp(line, ns, strlen(ns)) != 0 || line[strlen(ns)] != ' ') continue;
        char* r = strstr(line, write ? "write=" : "read=");
        if (r) {
            r += write ? 6 : 5;
            char list[160];
            size_t n = strcspn(r, " ");
            if (n >= sizeof(list)) n = sizeof(list) - 1;
            memcpy(list, r, n);
            list[n] = '\0';
            ok = list_has(list, principal);
        }
        if (!write && !ok) {
            /* writers can read */
            char* w = strstr(line, "write=");
            if (w) {
                w += 6;
                char list[160];
                size_t n = strcspn(w, " ");
                if (n >= sizeof(list)) n = sizeof(list) - 1;
                memcpy(list, w, n);
                list[n] = '\0';
                ok = list_has(list, principal);
            }
        }
        break;
    }
    fclose(fp);
    return ok;
}

/* ---------- caller authorization ---------- */

typedef struct {
    int  agent_id;
    char principal[32];
} caller_t;

static void get_caller(caller_t* c) {
    c->agent_id = intent_get_caller_agent();
    const char* p = intent_get_caller_principal();
    strncpy(c->principal, (p && p[0]) ? p : "system", sizeof(c->principal) - 1);
    c->principal[sizeof(c->principal) - 1] = '\0';
    if (c->agent_id == 0) strcpy(c->principal, "system");
}

static bool prefix_ok(const agent_path_rule_t* rules, uint32_t n, const char* ns) {
    for (uint32_t i = 0; i < n && i < MAX_PATH_RULES; i++) {
        const char* pre = rules[i].prefix;
        if (strcmp(pre, "*") == 0 || strncmp(ns, pre, strlen(pre)) == 0) return true;
    }
    return false;
}

static bool authorize(const caller_t* c, const char* ns, bool write) {
    if (strcmp(ns, "priv") != 0 && strncmp(ns, "shared:", 7) != 0) return false;
    if (c->agent_id > 0) {
        agent_info_t info;
        if (agent_info(c->agent_id, &info) != 0) return false;
        uint32_t cap = write ? CAP_MEM_WRITE : CAP_MEM_READ;
        if (!(info.caps.mask & cap)) return false;
        bool ok = write ? prefix_ok(info.caps.mem_write, info.caps.num_mem_write, ns)
                        : prefix_ok(info.caps.mem_read, info.caps.num_mem_read, ns);
        if (!ok) return false;
    }
    if (strncmp(ns, "shared:", 7) == 0) return shared_acl_ok(ns, c->principal, write);
    return true;
}

static void audit(const caller_t* c, uint32_t ev, const char* op, const char* ns, int id) {
    char d[64];
    snprintf(d, sizeof(d), "%s %.12s id:%d p:%.20s", op, ns, id, c->principal);
    audit_append(c->agent_id, ev, d);
}

/* ---------- storage ---------- */

static int load_ns(const char* file) {
    s_nslots = 0;
    memset(s_slots, 0, sizeof(s_slots));
    int fd = open(file, O_RDONLY);
    if (fd < 0) return 0;
    while (s_nslots < MAX_SLOTS) {
        int n = read(fd, &s_slots[s_nslots], SLOT_SIZE);
        if (n != SLOT_SIZE) break;
        s_nslots++;
    }
    close(fd);
    return 0;
}

static int write_slot(const char* dir, const char* file, int idx, const slot_t* s) {
    ensure_ns_dir(dir);
    int fd = open(file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (lseek(fd, (off_t)idx * SLOT_SIZE, SEEK_SET) < 0) { close(fd); return -1; }
    int n = write(fd, s, SLOT_SIZE);
    sync();
    close(fd);
    return n == SLOT_SIZE ? 0 : -1;
}

static int find_id(uint32_t id) {
    for (int i = 0; i < s_nslots; i++)
        if (slot_valid(&s_slots[i]) && s_slots[i].id == id) return i;
    return -1;
}

/* ---------- tokenizer / ranking ---------- */

static int tokenize(const char* text, uint32_t* out, int cap) {
    int n = 0;
    uint32_t h = 2166136261u;
    int len = 0;
    for (const char* p = text;; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            h = (h ^ (uint8_t)c) * 16777619u;
            len++;
        } else {
            if (len > 0 && n < cap) out[n++] = h;
            h = 2166136261u;
            len = 0;
            if (!c) break;
        }
    }
    return n;
}

static bool embed_text(const char* text, int8_t* vec) {
    if (s_call_fd < 0) return false;
    char args[TEXT_MAX * 6 + 64];
    json_buf_t jb;
    json_buf_init(&jb, args, sizeof(args));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "text");
    json_buf_append_str(&jb, text);
    json_buf_obj_end(&jb);
    if (jb.error) return false;

    static char res[4096];
    if (intent_call(s_call_fd, "model.embed", args, res, sizeof(res), 500) != 0) return false;
    char* v = strstr(res, "\"vector\"");
    if (!v) return false;
    v = strchr(v, '[');
    if (!v) return false;
    v++;
    for (int i = 0; i < VEC_DIM; i++) {
        char* end;
        double d = strtod(v, &end);
        if (end == v) return false;
        if (d > 1.0) d = 1.0;
        if (d < -1.0) d = -1.0;
        vec[i] = (int8_t)(d * 127.0);
        v = end;
        while (*v == ',' || *v == ' ') v++;
    }
    return true;
}

static float cosine8(const int8_t* a, const int8_t* b) {
    long dot = 0, na = 0, nb = 0;
    for (int i = 0; i < VEC_DIM; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0 || nb == 0) return 0.0f;
    return (float)dot / (sqrtf((float)na) * sqrtf((float)nb));
}

typedef struct { int idx; float score; } hit_t;

static int rank_slots(const char* query, int k, hit_t* hits) {
    uint32_t qt[16];
    int nq = tokenize(query, qt, 16);
    int8_t qvec[VEC_DIM];
    bool qv = embed_text(query, qvec);

    static float bm[MAX_SLOTS];
    int live = 0;
    float total_len = 0;
    int df[16];
    memset(df, 0, sizeof(df));
    static uint16_t dlen[MAX_SLOTS];
    static uint8_t tf[MAX_SLOTS][16];

    for (int i = 0; i < s_nslots; i++) {
        bm[i] = 0;
        dlen[i] = 0;
        memset(tf[i], 0, 16);
        if (!slot_valid(&s_slots[i])) continue;
        live++;
        uint32_t dt[96];
        int n = tokenize(s_slots[i].text, dt, 96);
        dlen[i] = (uint16_t)n;
        total_len += n;
        for (int t = 0; t < n; t++)
            for (int q = 0; q < nq; q++)
                if (dt[t] == qt[q] && tf[i][q] < 255) tf[i][q]++;
        for (int q = 0; q < nq; q++) if (tf[i][q]) df[q]++;
    }
    if (live == 0) return 0;
    float avgdl = total_len / live;
    if (avgdl < 1) avgdl = 1;
    float maxbm = 0;
    for (int i = 0; i < s_nslots; i++) {
        if (!slot_valid(&s_slots[i])) continue;
        float s = 0;
        for (int q = 0; q < nq; q++) {
            if (!tf[i][q]) continue;
            float idf = logf(1.0f + (live - df[q] + 0.5f) / (df[q] + 0.5f));
            float f = tf[i][q];
            s += idf * (f * 2.2f) / (f + 1.2f * (0.25f + 0.75f * dlen[i] / avgdl));
        }
        bm[i] = s;
        if (s > maxbm) maxbm = s;
    }

    int nh = 0;
    for (int i = 0; i < s_nslots; i++) {
        if (!slot_valid(&s_slots[i])) continue;
        float b = maxbm > 0 ? bm[i] / maxbm : 0;
        float score = b;
        if (qv && s_slots[i].vec_ok) {
            float c = cosine8(qvec, s_slots[i].vec);
            if (c < 0) c = 0;
            score = 0.6f * b + 0.4f * c;
            if (b == 0 && c < 0.5f) continue;   /* semantic-only hits need real similarity */
        } else if (b == 0) {
            continue;
        }
        /* insert into top-k */
        int pos = nh;
        if (nh < k) nh++; else if (score <= hits[k - 1].score) continue; else pos = k - 1;
        while (pos > 0 && hits[pos - 1].score < score) { hits[pos] = hits[pos - 1]; pos--; }
        hits[pos].idx = i;
        hits[pos].score = score;
    }
    return nh;
}

/* ---------- skill helpers ---------- */

static bool s_admin_override_dir;

static int err(char* eb, size_t cap, const char* msg) {
    snprintf(eb, cap, "%s", msg);
    return -1;
}

static const char* arg_ns(const json_val_t* root) {
    const char* ns = json_obj_get_str(root, "ns");
    return (ns && ns[0]) ? ns : "priv";
}

static void emit_slot(json_buf_t* jb, const slot_t* s, const char* ns, float score, bool with_score, bool trusted_view) {
    json_buf_obj_start(jb);
    json_buf_obj_key(jb, "id");
    json_buf_append_int(jb, s->id);
    json_buf_obj_key(jb, "ns");
    json_buf_append_str(jb, ns);
    json_buf_obj_key(jb, "provenance");
    json_buf_append_str(jb, s_prov_names[s->prov > 2 ? 2 : s->prov]);
    json_buf_obj_key(jb, "untrusted");
    json_buf_append_bool(jb, s->prov == PROV_UNTRUSTED);
    json_buf_obj_key(jb, "ts");
    json_buf_append_int(jb, s->ts);
    if (with_score) {
        char sc[24];
        snprintf(sc, sizeof(sc), "%.3f", score);
        json_buf_obj_key(jb, "score");
        json_buf_append_raw(jb, sc, strlen(sc));
    }
    json_buf_obj_key(jb, "tags");
    json_buf_append_str(jb, s->tags);
    json_buf_obj_key(jb, "text");
    json_buf_append_str(jb, s->text);
    (void)trusted_view;
    json_buf_obj_end(jb);
}

/* Core operations, parameterized by effective principal (admin may target any). */

static int do_put(const caller_t* c, const char* principal, const char* ns, const char* text, const char* tags,
                  int prov, char* res, size_t rcap, char* eb, size_t ecap) {
    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    size_t tl = strlen(text);
    if (tl == 0) return err(eb, ecap, "empty text");
    if (tl > TEXT_MAX) return err(eb, ecap, "text too long (max 383 bytes)");

    load_ns(file);
    int idx = -1;
    for (int i = 0; i < s_nslots; i++) {
        if (!slot_valid(&s_slots[i])) { idx = i; break; }
    }
    if (idx < 0) {
        if (s_nslots >= MAX_SLOTS) return err(eb, ecap, "namespace quota exceeded");
        idx = s_nslots;
    }
    uint32_t maxid = 0;
    for (int i = 0; i < s_nslots; i++) if (slot_valid(&s_slots[i]) && s_slots[i].id > maxid) maxid = s_slots[i].id;

    slot_t s;
    memset(&s, 0, sizeof(s));
    s.magic = SLOT_MAGIC;
    s.state = 1;
    s.prov = (uint8_t)prov;
    s.id = maxid + 1;
    s.ts = (uint32_t)time(NULL);
    s.tlen = (uint16_t)tl;
    memcpy(s.text, text, tl);
    if (tags) strncpy(s.tags, tags, sizeof(s.tags) - 1);
    s.vec_ok = embed_text(text, s.vec) ? 1 : 0;
    s.crc = slot_crc(&s);
    if (write_slot(dir, file, idx, &s) != 0) return err(eb, ecap, "write failed");

    audit(c, AUDIT_EVENT_MEM_PUT, "put", ns, (int)s.id);
    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "id");
    json_buf_append_int(&jb, s.id);
    json_buf_obj_key(&jb, "provenance");
    json_buf_append_str(&jb, s_prov_names[prov]);
    json_buf_obj_end(&jb);
    return 0;
}

static int do_search(const caller_t* c, const char* principal, const char* ns, const char* query, int k,
                     char* res, size_t rcap, char* eb, size_t ecap) {
    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    if (k < 1) k = 5;
    if (k > 10) k = 10;
    load_ns(file);
    hit_t hits[10];
    int nh = rank_slots(query, k, hits);
    audit(c, AUDIT_EVENT_MEM_SEARCH, "search", ns, nh);

    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "results");
    json_buf_arr_start(&jb);
    for (int i = 0; i < nh; i++) emit_slot(&jb, &s_slots[hits[i].idx], ns, hits[i].score, true, false);
    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);
    if (jb.error) return err(eb, ecap, "result too large");
    return 0;
}

static int do_list(const caller_t* c, const char* principal, const char* ns, char* res, size_t rcap, char* eb, size_t ecap) {
    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    load_ns(file);
    (void)c;
    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "entries");
    json_buf_arr_start(&jb);
    int count = 0;
    for (int i = 0; i < s_nslots; i++) {
        if (!slot_valid(&s_slots[i])) continue;
        if (jb.len + 600 > rcap) break;
        emit_slot(&jb, &s_slots[i], ns, 0, false, false);
        count++;
    }
    json_buf_arr_end(&jb);
    json_buf_obj_key(&jb, "count");
    json_buf_append_int(&jb, count);
    json_buf_obj_end(&jb);
    return 0;
}

static int do_get(const char* principal, const char* ns, int id, char* res, size_t rcap, char* eb, size_t ecap) {
    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    load_ns(file);
    int i = find_id((uint32_t)id);
    if (i < 0) return err(eb, ecap, "not found");
    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    emit_slot(&jb, &s_slots[i], ns, 0, false, false);
    return 0;
}

/* wipe == physically zero the slot (data is gone from the file, not just unlinked) */
static int do_forget(const caller_t* c, const char* principal, const char* ns, int id, char* res, size_t rcap,
                     char* eb, size_t ecap) {
    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    load_ns(file);
    int i = find_id((uint32_t)id);
    if (i < 0) return err(eb, ecap, "not found");
    slot_t z;
    memset(&z, 0, sizeof(z));
    if (write_slot(dir, file, i, &z) != 0) return err(eb, ecap, "write failed");
    audit(c, AUDIT_EVENT_MEM_FORGET, "forget", ns, id);
    snprintf(res, rcap, "{\"forgotten\":true,\"id\":%d}", id);
    return 0;
}

/* ---------- skill handlers ---------- */

static int h_put(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    caller_t c; get_caller(&c);
    const char* ns = arg_ns(doc.root);
    if (!authorize(&c, ns, true)) {
        audit(&c, AUDIT_EVENT_DENIED, "put-denied", ns, 0);
        return err(eb, ecap, "denied: no CAP_MEM_WRITE / ACL for namespace");
    }
    const char* text = json_obj_get_str(doc.root, "text");
    if (!text) return err(eb, ecap, "missing 'text'");
    /* Provenance is decided by the daemon from the caller's identity:
       humans/system (agent 0) may mark 'user'; agents are 'agent' and may only downgrade to 'untrusted'. */
    int prov = (c.agent_id == 0) ? PROV_USER : PROV_AGENT;
    const char* pv = json_obj_get_str(doc.root, "provenance");
    if (pv && strcmp(pv, "untrusted") == 0) prov = PROV_UNTRUSTED;
    else if (pv && strcmp(pv, "agent") == 0) prov = PROV_AGENT;
    bool from_tool = false;
    json_obj_get_bool(doc.root, "from_tool", &from_tool);
    if (from_tool) prov = PROV_UNTRUSTED;
    return do_put(&c, c.principal, ns, text, json_obj_get_str(doc.root, "tags"), prov, res, rcap, eb, ecap);
}

static int h_search(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    caller_t c; get_caller(&c);
    const char* ns = arg_ns(doc.root);
    if (!authorize(&c, ns, false)) {
        audit(&c, AUDIT_EVENT_DENIED, "search-denied", ns, 0);
        return err(eb, ecap, "denied: no CAP_MEM_READ / ACL for namespace");
    }
    const char* q = json_obj_get_str(doc.root, "query");
    if (!q) return err(eb, ecap, "missing 'query'");
    int k = 5;
    json_obj_get_int(doc.root, "k", &k);
    return do_search(&c, c.principal, ns, q, k, res, rcap, eb, ecap);
}

static int h_get(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    caller_t c; get_caller(&c);
    const char* ns = arg_ns(doc.root);
    if (!authorize(&c, ns, false)) return err(eb, ecap, "denied: no CAP_MEM_READ / ACL for namespace");
    int id = 0;
    if (json_obj_get_int(doc.root, "id", &id) != 0) return err(eb, ecap, "missing 'id'");
    return do_get(c.principal, ns, id, res, rcap, eb, ecap);
}

static int h_list(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    caller_t c; get_caller(&c);
    const char* ns = arg_ns(doc.root);
    if (!authorize(&c, ns, false)) return err(eb, ecap, "denied: no CAP_MEM_READ / ACL for namespace");
    return do_list(&c, c.principal, ns, res, rcap, eb, ecap);
}

static int h_forget(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    caller_t c; get_caller(&c);
    const char* ns = arg_ns(doc.root);
    if (!authorize(&c, ns, true)) {
        audit(&c, AUDIT_EVENT_DENIED, "forget-denied", ns, 0);
        return err(eb, ecap, "denied: no CAP_MEM_WRITE / ACL for namespace");
    }
    int id = 0;
    if (json_obj_get_int(doc.root, "id", &id) != 0) return err(eb, ecap, "missing 'id'");
    return do_forget(&c, c.principal, ns, id, res, rcap, eb, ecap);
}

static int h_stats(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)args; (void)u; (void)eb; (void)ecap;
    caller_t c; get_caller(&c);
    char dir[96], file[128];
    int live = 0;
    if (ns_paths(c.principal, "priv", dir, sizeof(dir), file, sizeof(file)) == 0) {
        load_ns(file);
        for (int i = 0; i < s_nslots; i++) if (slot_valid(&s_slots[i])) live++;
    }
    snprintf(res, rcap, "{\"principal\":\"%s\",\"priv_entries\":%d,\"quota\":%d,\"embeddings\":%s}",
             c.principal, live, MAX_SLOTS, s_call_fd >= 0 ? "true" : "false");
    return 0;
}

/* mem.admin - human only (agent_id must be 0, i.e. a non-agent process). */
static int h_admin(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    caller_t c; get_caller(&c);
    if (c.agent_id != 0) {
        audit(&c, AUDIT_EVENT_DENIED, "admin-denied", "-", 0);
        return err(eb, ecap, "mem.admin is restricted to human (non-agent) callers");
    }
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) return err(eb, ecap, "invalid JSON");
    const char* op = json_obj_get_str(doc.root, "op");
    const char* principal = json_obj_get_str(doc.root, "principal");
    const char* ns = arg_ns(doc.root);
    if (!op || !principal) return err(eb, ecap, "missing 'op'/'principal'");
    int id = 0;
    json_obj_get_int(doc.root, "id", &id);
    audit(&c, AUDIT_EVENT_MEM_ADMIN, op, ns, id);

    if (strcmp(op, "list") == 0) return do_list(&c, principal, ns, res, rcap, eb, ecap);
    if (strcmp(op, "get") == 0) return do_get(principal, ns, id, res, rcap, eb, ecap);
    if (strcmp(op, "search") == 0) {
        const char* q = json_obj_get_str(doc.root, "query");
        if (!q) return err(eb, ecap, "missing 'query'");
        return do_search(&c, principal, ns, q, 5, res, rcap, eb, ecap);
    }
    if (strcmp(op, "delete") == 0) return do_forget(&c, principal, ns, id, res, rcap, eb, ecap);
    if (strcmp(op, "put") == 0) {
        const char* text = json_obj_get_str(doc.root, "text");
        if (!text) return err(eb, ecap, "missing 'text'");
        return do_put(&c, principal, ns, text, json_obj_get_str(doc.root, "tags"), PROV_USER, res, rcap, eb, ecap);
    }

    char dir[96], file[128];
    if (ns_paths(principal, ns, dir, sizeof(dir), file, sizeof(file)) != 0) return err(eb, ecap, "bad namespace");
    load_ns(file);

    if (strcmp(op, "mark-trusted") == 0) {
        int i = find_id((uint32_t)id);
        if (i < 0) return err(eb, ecap, "not found");
        slot_t s = s_slots[i];
        s.prov = PROV_USER;
        s.crc = slot_crc(&s);
        if (write_slot(dir, file, i, &s) != 0) return err(eb, ecap, "write failed");
        snprintf(res, rcap, "{\"marked\":true,\"id\":%d}", id);
        return 0;
    }
    if (strcmp(op, "wipe") == 0) {
        slot_t z;
        memset(&z, 0, sizeof(z));
        int n = 0;
        for (int i = 0; i < s_nslots; i++) {
            if (s_slots[i].magic == SLOT_MAGIC || s_slots[i].state) {
                if (write_slot(dir, file, i, &z) != 0) return err(eb, ecap, "write failed");
                n++;
            }
        }
        snprintf(res, rcap, "{\"wiped\":%d}", n);
        return 0;
    }
    if (strcmp(op, "stats") == 0) {
        int live = 0, untrusted = 0;
        for (int i = 0; i < s_nslots; i++) {
            if (!slot_valid(&s_slots[i])) continue;
            live++;
            if (s_slots[i].prov == PROV_UNTRUSTED) untrusted++;
        }
        snprintf(res, rcap, "{\"entries\":%d,\"untrusted\":%d,\"quota\":%d}", live, untrusted, MAX_SLOTS);
        return 0;
    }
    return err(eb, ecap, "unknown admin op");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);   /* prevent death if intentd pipe breaks */
    (void)s_admin_override_dir;
    ensure_dir("/var");
    ensure_dir("/var/mem");

    int fd = -1;
    for (int tries = 0; tries < 50 && fd < 0; tries++) {
        fd = intent_connect();
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) {
        fprintf(stderr, "memd: cannot connect to intent bus\n");
        return 1;
    }
    s_call_fd = intent_connect();   /* separate connection for outbound model.embed calls */

    const char* sch_obj = "{\"type\":\"object\"}";
    const char* sch_put = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}";
    const char* sch_q = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}";
    const char* sch_id = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"]}";
    const char* sch_admin = "{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"principal\":{\"type\":\"string\"}},\"required\":[\"op\",\"principal\"]}";

    intent_register_skill(fd, "mem.put", "Store a memory (ns: priv or shared:<name>)", sch_put, NULL, false, h_put, NULL);
    intent_register_skill(fd, "mem.search", "Hybrid BM25+embedding memory search", sch_q, NULL, false, h_search, NULL);
    intent_register_skill(fd, "mem.get", "Fetch a memory by id", sch_id, NULL, false, h_get, NULL);
    intent_register_skill(fd, "mem.forget", "Physically delete a memory by id", sch_id, NULL, false, h_forget, NULL);
    intent_register_skill(fd, "mem.list", "List memories in a namespace", sch_obj, NULL, false, h_list, NULL);
    intent_register_skill(fd, "mem.stats", "Memory usage statistics", sch_obj, NULL, false, h_stats, NULL);
    intent_register_skill(fd, "mem.admin", "Human-only memory administration", sch_admin, NULL, false, h_admin, NULL);

    printf("memd: serving mem.* skills (embeddings %s)\n", s_call_fd >= 0 ? "on" : "off");
    fflush(stdout);
    intent_serve(fd);
    intent_close(fd);
    if (s_call_fd >= 0) intent_close(s_call_fd);
    return 0;
}

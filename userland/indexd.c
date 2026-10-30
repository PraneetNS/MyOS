/*
 * indexd - semantic file indexer (Phase 5).
 *
 * Runs as a bus provider (fs.search, fs.index_status). On startup it forks a
 * low-priority crawler child (nice 19, sleeps between files) that indexes the
 * roots listed in /etc/index.conf into /var/index/index.dat (CRC-protected
 * 512-byte slots; torn/partial slots are ignored). The parent answers queries.
 *
 * Security: the index covers files the *daemon* can read, so every query is
 * filtered by the caller's kernel-vouched CAP_FS_READ prefixes at query time.
 * Callers without CAP_FS_READ (or with no matching prefix) never see a path,
 * snippet or even a hit count for files outside their allowlist.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <agent.h>
#include <intent.h>
#include <json.h>

#define SLOT_SIZE  512
#define MAGIC      0x31584449u
#define MAX_CHUNKS 768
#define CHUNK_TXT  240
#define PATH_MAX_I 128
#define VEC_DIM    64
#define MAX_FILE   8192
#define SCAN_SECS  20
#define INDEX_DIR  "/var/index"
#define INDEX_FILE "/var/index/index.dat"
#define STATUS_FILE "/var/index/status"

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t mtime;
    uint32_t fsize;
    uint16_t chunk_no;
    uint16_t tlen;
    uint8_t  vec_ok;
    uint8_t  pad[3];
    char     path[PATH_MAX_I];
    int8_t   vec[VEC_DIM];
    char     text[CHUNK_TXT];
    uint8_t  reserved[SLOT_SIZE - 4 - 4 - 4 - 4 - 2 - 2 - 4 - PATH_MAX_I - VEC_DIM - CHUNK_TXT];
} chunk_t;

typedef char chunk_size_check[(sizeof(chunk_t) == SLOT_SIZE) ? 1 : -1];

static chunk_t s_chunks[MAX_CHUNKS];
static int s_n;
static chunk_t s_new[MAX_CHUNKS];
static int s_nn;
static int s_embed_fd = -1;

static uint32_t crc32_buf(const void* data, size_t n) {
    const uint8_t* p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (-(int32_t)(c & 1)));
    }
    return ~c;
}

static uint32_t chunk_crc(const chunk_t* c) {
    chunk_t t = *c;
    t.crc = 0;
    return crc32_buf(&t, sizeof(t));
}

static bool chunk_ok(const chunk_t* c) {
    return c->magic == MAGIC && c->tlen < CHUNK_TXT && c->crc == chunk_crc(c);
}

/* ---------- tokenizer / embeddings (shared approach with memd) ---------- */

static int tokenize(const char* text, uint32_t* out, int cap) {
    int n = 0, len = 0;
    uint32_t h = 2166136261u;
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
    if (s_embed_fd < 0) return false;
    char args[CHUNK_TXT * 6 + 64];
    json_buf_t jb;
    json_buf_init(&jb, args, sizeof(args));
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "text");
    json_buf_append_str(&jb, text);
    json_buf_obj_end(&jb);
    if (jb.error) return false;
    static char res[4096];
    if (intent_call(s_embed_fd, "model.embed", args, res, sizeof(res), 2000) != 0) return false;
    char* v = strstr(res, "\"vector\"");
    if (!v || !(v = strchr(v, '['))) return false;
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
    for (int i = 0; i < VEC_DIM; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
    if (!na || !nb) return 0.0f;
    return (float)dot / (sqrtf((float)na) * sqrtf((float)nb));
}

/* ---------- storage ---------- */

static void save_index(void) {
    mkdir("/var", 0755);
    mkdir(INDEX_DIR, 0755);
    int fd = open(INDEX_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    for (int i = 0; i < s_n; i++) write(fd, &s_chunks[i], SLOT_SIZE);
    close(fd);
    sync();
}

static int load_index(chunk_t* dst, int cap) {
    int fd = open(INDEX_FILE, O_RDONLY);
    if (fd < 0) return 0;
    int n = 0;
    chunk_t c;
    while (n < cap && read(fd, &c, SLOT_SIZE) == SLOT_SIZE) {
        if (chunk_ok(&c)) dst[n++] = c;
    }
    close(fd);
    return n;
}

static void write_status(int files, int chunks, bool scanning) {
    mkdir("/var", 0755);
    mkdir(INDEX_DIR, 0755);
    int fd = open(STATUS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "files=%d chunks=%d scanning=%d time=%u\n", files, chunks, scanning ? 1 : 0, (unsigned)time(NULL));
    write(fd, buf, n);
    close(fd);
}

/* ---------- crawler (child) ---------- */

static bool text_like(const char* buf, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32)) return false;
    }
    return true;
}

static bool have_unchanged(const char* path, uint32_t mtime, uint32_t size) {
    bool found = false;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_chunks[i].path, path) == 0) {
            if (s_chunks[i].mtime != mtime || s_chunks[i].fsize != size) return false;
            found = true;
        }
    }
    return found;
}

static int s_files_seen;

static void index_file(const char* path, const struct stat* st) {
    if (s_nn >= MAX_CHUNKS) return;
    if (strlen(path) >= PATH_MAX_I) return;
    uint32_t mtime = (uint32_t)st->st_mtime, size = (uint32_t)st->st_size;
    s_files_seen++;

    if (have_unchanged(path, mtime, size)) {          /* incremental: carry over */
        for (int i = 0; i < s_n && s_nn < MAX_CHUNKS; i++)
            if (strcmp(s_chunks[i].path, path) == 0) s_new[s_nn++] = s_chunks[i];
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    static char buf[MAX_FILE + 1];
    int n = read(fd, buf, MAX_FILE);
    close(fd);
    if (n <= 0 || !text_like(buf, n)) return;
    buf[n] = '\0';

    int off = 0, chunk_no = 0;
    while (off < n && chunk_no < 16 && s_nn < MAX_CHUNKS) {
        int len = n - off;
        if (len > CHUNK_TXT - 1) {
            len = CHUNK_TXT - 1;
            int cut = len;
            while (cut > len / 2 && buf[off + cut] != '\n' && buf[off + cut] != ' ') cut--;
            if (cut > len / 2) len = cut;
        }
        chunk_t* c = &s_new[s_nn];
        memset(c, 0, sizeof(*c));
        c->magic = MAGIC;
        c->mtime = mtime;
        c->fsize = size;
        c->chunk_no = (uint16_t)chunk_no;
        c->tlen = (uint16_t)len;
        memcpy(c->text, buf + off, len);
        strcpy(c->path, path);
        c->vec_ok = embed_text(c->text, c->vec) ? 1 : 0;
        c->crc = chunk_crc(c);
        s_nn++;
        off += len;
        while (off < n && (buf[off] == '\n' || buf[off] == ' ')) off++;
        chunk_no++;
        usleep(20000);                                 /* throttle */
    }
}

static void crawl_dir(const char* dir, int depth) {
    if (depth > 6) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_I + 32];
        snprintf(path, sizeof(path), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) crawl_dir(path, depth + 1);
        else if (S_ISREG(st.st_mode)) index_file(path, &st);
        usleep(10000);
    }
    closedir(d);
}

static void crawler_main(void) {
    nice(19);
    s_embed_fd = intent_connect();
    s_n = load_index(s_chunks, MAX_CHUNKS);
    for (;;) {
        write_status(s_files_seen, s_n, true);
        s_nn = 0;
        s_files_seen = 0;
        FILE* fp = fopen("/etc/index.conf", "r");
        if (fp) {
            char line[PATH_MAX_I];
            while (fgets(line, sizeof(line), fp)) {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
                if (line[0] == '/' ) crawl_dir(line, 0);
            }
            fclose(fp);
        }
        memcpy(s_chunks, s_new, (size_t)s_nn * sizeof(chunk_t));
        s_n = s_nn;
        save_index();
        write_status(s_files_seen, s_n, false);
        sleep(SCAN_SECS);
    }
}

/* ---------- query side ---------- */

static bool path_allowed(const agent_info_t* info, const char* path) {
    for (uint32_t i = 0; i < info->caps.num_fs_read && i < MAX_PATH_RULES; i++) {
        const char* pre = info->caps.fs_read[i].prefix;
        size_t pl = strlen(pre);
        if (pl == 0) continue;
        if (strcmp(pre, "/") == 0) return true;
        if (strncmp(path, pre, pl) == 0 && (path[pl] == '\0' || path[pl] == '/' || pre[pl - 1] == '/')) return true;
    }
    return false;
}

typedef struct { int idx; float score; } hit_t;

static int h_search(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) { snprintf(eb, ecap, "invalid JSON"); return -1; }
    const char* q = json_obj_get_str(doc.root, "query");
    if (!q) { snprintf(eb, ecap, "missing 'query'"); return -1; }
    int k = 5;
    json_obj_get_int(doc.root, "k", &k);
    if (k < 1) k = 5;
    if (k > 8) k = 8;

    int agent = intent_get_caller_agent();
    agent_info_t info;
    bool unconfined = (agent == 0);
    if (!unconfined) {
        if (agent_info(agent, &info) != 0 || !(info.caps.mask & CAP_FS_READ)) {
            audit_append(agent, AUDIT_EVENT_DENIED, "fs.search without CAP_FS_READ");
            snprintf(eb, ecap, "denied: caller lacks CAP_FS_READ");
            return -1;
        }
    }

    s_n = load_index(s_chunks, MAX_CHUNKS);

    uint32_t qt[16];
    int nq = tokenize(q, qt, 16);
    int8_t qvec[VEC_DIM];
    bool qv = embed_text(q, qvec);

    static float bm[MAX_CHUNKS];
    static uint16_t dlen[MAX_CHUNKS];
    static uint8_t tf[MAX_CHUNKS][16];
    static bool vis[MAX_CHUNKS];
    int df[16];
    memset(df, 0, sizeof(df));
    int live = 0;
    float total = 0;

    /* Filter FIRST: statistics (df, avgdl) are computed only over visible chunks,
       so ranking cannot leak information about files the caller may not read. */
    for (int i = 0; i < s_n; i++) {
        vis[i] = unconfined || path_allowed(&info, s_chunks[i].path);
        bm[i] = 0; dlen[i] = 0;
        memset(tf[i], 0, 16);
        if (!vis[i]) continue;
        live++;
        uint32_t dt[64];
        int n = tokenize(s_chunks[i].text, dt, 64);
        dlen[i] = (uint16_t)n;
        total += n;
        for (int t = 0; t < n; t++)
            for (int j = 0; j < nq; j++)
                if (dt[t] == qt[j] && tf[i][j] < 255) tf[i][j]++;
        for (int j = 0; j < nq; j++) if (tf[i][j]) df[j]++;
    }

    hit_t hits[8];
    int nh = 0;
    if (live > 0) {
        float avgdl = total / live;
        if (avgdl < 1) avgdl = 1;
        float maxbm = 0;
        for (int i = 0; i < s_n; i++) {
            if (!vis[i]) continue;
            float s = 0;
            for (int j = 0; j < nq; j++) {
                if (!tf[i][j]) continue;
                float idf = logf(1.0f + (live - df[j] + 0.5f) / (df[j] + 0.5f));
                float f = tf[i][j];
                s += idf * (f * 2.2f) / (f + 1.2f * (0.25f + 0.75f * dlen[i] / avgdl));
            }
            bm[i] = s;
            if (s > maxbm) maxbm = s;
        }
        for (int i = 0; i < s_n; i++) {
            if (!vis[i]) continue;
            float b = maxbm > 0 ? bm[i] / maxbm : 0;
            float score = b;
            if (qv && s_chunks[i].vec_ok) {
                float c = cosine8(qvec, s_chunks[i].vec);
                if (c < 0) c = 0;
                score = 0.6f * b + 0.4f * c;
                if (b == 0 && c < 0.5f) continue;
            } else if (b == 0) continue;
            int pos = nh;
            if (nh < k) nh++; else if (score <= hits[k - 1].score) continue; else pos = k - 1;
            while (pos > 0 && hits[pos - 1].score < score) { hits[pos] = hits[pos - 1]; pos--; }
            hits[pos].idx = i;
            hits[pos].score = score;
        }
    }

    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "results");
    json_buf_arr_start(&jb);
    for (int h = 0; h < nh; h++) {
        const chunk_t* c = &s_chunks[hits[h].idx];
        char sc[24];
        snprintf(sc, sizeof(sc), "%.3f", hits[h].score);
        json_buf_obj_start(&jb);
        json_buf_obj_key(&jb, "path");
        json_buf_append_str(&jb, c->path);
        json_buf_obj_key(&jb, "chunk");
        json_buf_append_int(&jb, c->chunk_no);
        json_buf_obj_key(&jb, "score");
        json_buf_append_raw(&jb, sc, strlen(sc));
        json_buf_obj_key(&jb, "untrusted");
        json_buf_append_bool(&jb, true);   /* file content is data, never instructions */
        json_buf_obj_key(&jb, "snippet");
        json_buf_append_str(&jb, c->text);
        json_buf_obj_end(&jb);
    }
    json_buf_arr_end(&jb);
    json_buf_obj_end(&jb);
    if (jb.error) { snprintf(eb, ecap, "result too large"); return -1; }
    return 0;
}

static int h_status(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)args; (void)eb; (void)ecap; (void)u;
    char line[96] = "files=0 chunks=0 scanning=0";
    int fd = open(STATUS_FILE, O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, line, sizeof(line) - 1);
        if (n < 0) n = 0;
        line[n] = '\0';
        close(fd);
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
    }
    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "status");
    json_buf_append_str(&jb, line);
    json_buf_obj_end(&jb);
    return 0;
}

int main(void) {
    mkdir("/var", 0755);
    mkdir(INDEX_DIR, 0755);

    int fd = -1;
    for (int t = 0; t < 50 && fd < 0; t++) {
        fd = intent_connect();
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) { fprintf(stderr, "indexd: cannot connect to intent bus\n"); return 1; }
    s_embed_fd = intent_connect();

    pid_t child = fork();
    if (child == 0) {
        intent_close(fd);
        crawler_main();
        _exit(0);
    }

    const char* sch_q = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}";
    intent_register_skill(fd, "fs.search", "Semantic search over indexed files (filtered by caller CAP_FS_READ)", sch_q, NULL, false, h_search, NULL);
    intent_register_skill(fd, "fs.index_status", "Indexer status", "{\"type\":\"object\"}", NULL, false, h_status, NULL);

    printf("indexd: serving fs.search\n");
    fflush(stdout);
    intent_serve(fd);
    intent_close(fd);
    if (child > 0) { kill(child, 9); waitpid(child, NULL, 0); }
    return 0;
}


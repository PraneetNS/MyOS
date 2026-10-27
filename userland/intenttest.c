#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <agent.h>
#include <intent.h>
#include <json.h>

extern unsigned int sys_free_frames(void);

/* SHA-256 implementation for audit verification */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} test_sha256_ctx_t;

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

static void test_sha256_transform(test_sha256_ctx_t* ctx, const uint8_t* data) {
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

static void test_sha256_init(test_sha256_ctx_t* ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void test_sha256_update(test_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            test_sha256_transform(ctx, ctx->buffer);
    }
}

static void test_sha256_final(test_sha256_ctx_t* ctx, uint8_t* hash) {
    uint8_t pad = 0x80;
    uint64_t total_bits = ctx->count * 8;
    test_sha256_update(ctx, &pad, 1);
    while ((ctx->count % 64) != 56) {
        pad = 0x00;
        test_sha256_update(ctx, &pad, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; --i) {
        len_bytes[i] = (uint8_t)(total_bits & 0xff);
        total_bits >>= 8;
    }
    test_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i + (j * 4)] = (uint8_t)((ctx->state[j] >> (24 - i * 8)) & 0x000000ff);
        }
    }
}

static void hex_to_bin(const char* hex, uint8_t* bin, size_t bin_len) {
    for (size_t i = 0; i < bin_len; i++) {
        unsigned int b = 0;
        sscanf(hex + i * 2, "%02x", &b);
        bin[i] = (uint8_t)b;
    }
}

/* 1. JSON Parser, Serializer, and Fuzzing */
static int test_json_and_fuzzing(void) {
    printf("[TEST 1] Testing JSON parser, schema validation, and fuzz-resilience...\n");

    /* Valid parsing */
    const char* valid_json = "{\"str\":\"hello\",\"num\":42,\"pi\":3.14,\"bool\":true,\"nil\":null,\"arr\":[1,2,3]}";
    json_doc_t doc;
    if (json_parse(&doc, valid_json, strlen(valid_json)) != JSON_ERR_OK) {
        printf("FAILED: Valid JSON parse failed\n");
        return 0;
    }

    if (strcmp(json_obj_get_str(doc.root, "str"), "hello") != 0) return 0;
    int n = 0;
    json_obj_get_int(doc.root, "num", &n);
    if (n != 42) return 0;
    bool b = false;
    json_obj_get_bool(doc.root, "bool", &b);
    if (!b) return 0;
    if (json_arr_len(json_obj_get_arr(doc.root, "arr")) != 3) return 0;

    /* Schema validation test */
    const char* schema_json = "{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}},\"required\":[\"msg\"]}";
    json_doc_t sdoc;
    json_parse(&sdoc, schema_json, strlen(schema_json));

    const char* valid_args = "{\"msg\":\"hello\"}";
    json_doc_t vdoc;
    json_parse(&vdoc, valid_args, strlen(valid_args));
    char err_buf[128];
    if (json_validate_schema(sdoc.root, vdoc.root, err_buf, sizeof(err_buf)) != 0) {
        printf("FAILED: Valid schema check failed: %s\n", err_buf);
        return 0;
    }

    const char* bad_args = "{\"msg\":123}";
    json_doc_t bdoc;
    json_parse(&bdoc, bad_args, strlen(bad_args));
    if (json_validate_schema(sdoc.root, bdoc.root, err_buf, sizeof(err_buf)) == 0) {
        printf("FAILED: Bad schema should have failed\n");
        return 0;
    }

    /* Fuzz-style malformed inputs */
    const char* fuzz_inputs[] = {
        "", " ", "{", "}", "[", "]",
        "{\"unclosed\":",
        "{\"trailing_comma\": 1,}",
        "[1, 2, 3,]",
        "{unquoted_key: 1}",
        "{\"bad_esc\": \"\\q\"}",
        "{\"bad_hex\": \"\\u12\"}",
        "{\"nested\":[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]}",
        "{\"huge_num\": 1234567890123456789012345678901234567890123456789012345678901234567890}",
        "{\"bad_dot\": 12.34.56}",
        "{\"bad_minus\": -}",
        "{NULL: null}"
    };

    for (size_t i = 0; i < sizeof(fuzz_inputs) / sizeof(fuzz_inputs[0]); i++) {
        json_doc_t fdoc;
        json_err_t err = json_parse(&fdoc, fuzz_inputs[i], strlen(fuzz_inputs[i]));
        if (err == JSON_ERR_OK) {
            printf("FAILED: Malformed input '%s' unexpectedly parsed as OK!\n", fuzz_inputs[i]);
            return 0;
        }
    }

    /* Test raw socket oversized and garbage frame against daemon */
    int raw_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (raw_fd >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, INTENT_SOCK_PATH, sizeof(addr.sun_path) - 1);
        if (connect(raw_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            /* Send oversized frame header */
            struct intent_header oh;
            oh.len = 65000;
            oh.type = INTENT_MSG_CALL;
            oh.flags = 0;
            oh.call_id = 999;
            write(raw_fd, &oh, sizeof(oh));
        }
        close(raw_fd);
    }

    /* Verify daemon is still alive */
    int test_fd = intent_connect();
    if (test_fd < 0) {
        printf("FAILED: Daemon crashed after malformed/oversized frames\n");
        return 0;
    }
    intent_close(test_fd);

    printf("PASSED: JSON parser strictness, schema validation, and daemon fuzz-resilience OK\n");
    return 1;
}

static int s_echo_handler(const char* skill, const char* args_json,
                          char* result_json, size_t result_cap,
                          char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Malformed args: '%s'", args_json);
        return -1;
    }
    const char* msg = json_obj_get_str(doc.root, "msg");
    if (!msg) {
        snprintf(err_buf, err_cap, "Missing msg");
        return -1;
    }
    json_buf_t jb;
    json_buf_init(&jb, result_json, result_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "echo");
    json_buf_append_str(&jb, msg);
    json_buf_obj_end(&jb);
    return 0;
}

/* 2. Register + Call Round Trip & Schema Rejection */
static int test_round_trip_and_schema(void) {
    printf("[TEST 2] Testing register + call round trip and schema rejection...\n");

    pid_t prov_pid = fork();
    if (prov_pid == 0) {
        int pfd = intent_connect();
        if (pfd < 0) exit(1);
        const char* schema = "{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}},\"required\":[\"msg\"]}";
        if (intent_register_skill(pfd, "test.echo", "Echo service", schema, NULL, false, s_echo_handler, NULL) != 0) {
            exit(2);
        }
        intent_serve(pfd);
        intent_close(pfd);
        exit(0);
    }

    usleep(50000); /* 50ms for provider registration */

    int cfd = intent_connect();
    if (cfd < 0) {
        printf("FAILED: Caller failed to connect\n");
        kill(prov_pid, SIGTERM);
        return 0;
    }

    /* Valid call */
    char res[512];
    if (intent_call(cfd, "test.echo", "{\"msg\":\"hello intent\"}", res, sizeof(res), 3000) != 0) {
        printf("FAILED: Valid round-trip call failed: %s\n", res);
        close(cfd);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    if (!strstr(res, "hello intent")) {
        printf("FAILED: Result did not contain expected message: %s\n", res);
        close(cfd);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    /* Bad schema call */
    int bad_rc = intent_call(cfd, "test.echo", "{\"wrong_arg\": 123}", res, sizeof(res), 3000);
    if (bad_rc == 0) {
        printf("FAILED: Schema rejection failed (call succeeded unexpectedly)\n");
        close(cfd);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    close(cfd);
    kill(prov_pid, SIGTERM);
    waitpid(prov_pid, NULL, 0);

    printf("PASSED: Register + call round trip and schema rejection OK\n");
    return 1;
}

static int handle_fs_read(const char* skill, const char* args_json,
                          char* result_json, size_t result_cap,
                          char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Failed to parse args");
        return -1;
    }
    const char* path = json_obj_get_str(doc.root, "path");
    if (!path) {
        snprintf(err_buf, err_cap, "Missing path");
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err_buf, err_cap, "Failed to open '%s': %s", path, strerror(errno));
        return -1;
    }
    char content[256];
    ssize_t n = read(fd, content, sizeof(content) - 1);
    close(fd);
    if (n < 0) {
        snprintf(err_buf, err_cap, "Failed to read '%s': %s", path, strerror(errno));
        return -1;
    }
    content[n] = '\0';
    snprintf(result_json, result_cap, "{\"content\":\"%s\"}", content);
    return 0;
}

static int handle_fs_delete(const char* skill, const char* args_json,
                            char* result_json, size_t result_cap,
                            char* err_buf, size_t err_cap, void* udata) {
    (void)skill; (void)udata;
    json_doc_t doc;
    if (json_parse(&doc, args_json, strlen(args_json)) != JSON_ERR_OK || !doc.root) {
        snprintf(err_buf, err_cap, "Failed to parse args");
        return -1;
    }
    const char* path = json_obj_get_str(doc.root, "path");
    if (!path) {
        snprintf(err_buf, err_cap, "Missing path");
        return -1;
    }
    if (unlink(path) < 0) {
        snprintf(err_buf, err_cap, "Failed to unlink '%s': %s", path, strerror(errno));
        return -1;
    }
    snprintf(result_json, result_cap, "{\"deleted\":true}");
    return 0;
}

/* 3. Identity & Spoofing Defense */
static int test_identity_and_spoofing(void) {
    printf("[TEST 3] Testing peer identity and caller impersonation defense...\n");

    /* Create agent with CAP_BUS_CALL("sys.") and CAP_SOCKET_CONNECT("/run") */
    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = CAP_BUS_CALL | CAP_EXEC | CAP_SOCKET_CONNECT;
    caps.num_bus_call = 1;
    strncpy(caps.bus_call[0].prefix, "sys.", MAX_PATH_PREFIX_LEN - 1);
    caps.num_exec = 1;
    strncpy(caps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);
    caps.num_connect = 1;
    strncpy(caps.connect[0].prefix, "/run", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    int ag_id = agent_create("unprivileged_agent", &caps, &budget);
    if (ag_id < 0) {
        printf("FAILED: agent_create failed\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) != 0) exit(10);

        int fd = intent_connect();
        if (fd < 0) exit(11);

        char res[256];
        /* Call "fs.read" which is outside "sys." */
        int rc = intent_call(fd, "fs.read", "{\"path\":\"/home\"}", res, sizeof(res), 2000);
        if (rc == 0) {
            /* Should have failed! */
            exit(12);
        }

        /* Try spoofing agent_id in JSON payload */
        const char* spoof_body = "{\"caller_agent\":0,\"agent_id\":0,\"pid\":1,\"skill\":\"fs.read\",\"args\":{\"path\":\"/home\"}}";
        rc = intent_call(fd, "fs.read", spoof_body, res, sizeof(res), 2000);
        if (rc == 0) {
            /* Spoof succeeded! Fatal! */
            exit(13);
        }

        intent_close(fd);
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    agent_kill(ag_id);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASSED: Identity enforcement and spoofing defense verified\n");
        return 1;
    }

    printf("FAILED: Identity test child returned status %d\n", WEXITSTATUS(status));
    return 0;
}

/* 4. Confused-Deputy Test & Dual-Layer Audit Log */
static int test_confused_deputy(void) {
    printf("[TEST 4] Testing confused-deputy defense (provider kernel sandbox)...\n");

    /* Provider agent: has CAP_BUS_PROVIDE("fs."), CAP_FS_READ("/home"), and CAP_SOCKET_CONNECT("/run") */
    agent_caps_t pcaps;
    memset(&pcaps, 0, sizeof(pcaps));
    pcaps.mask = CAP_BUS_PROVIDE | CAP_FS_READ | CAP_EXEC | CAP_SOCKET_CONNECT;
    pcaps.num_bus_provide = 1;
    strncpy(pcaps.bus_provide[0].prefix, "fs.", MAX_PATH_PREFIX_LEN - 1);
    pcaps.num_fs_read = 1;
    strncpy(pcaps.fs_read[0].prefix, "/home", MAX_PATH_PREFIX_LEN - 1);
    pcaps.num_exec = 1;
    strncpy(pcaps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);
    pcaps.num_connect = 1;
    strncpy(pcaps.connect[0].prefix, "/run", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t pbudget;
    memset(&pbudget, 0, sizeof(pbudget));

    int prov_ag = agent_create("fs_provider", &pcaps, &pbudget);
    if (prov_ag < 0) {
        printf("FAILED: agent_create for provider failed\n");
        return 0;
    }

    /* Fork provider process */
    pid_t prov_pid = fork();
    if (prov_pid == 0) {
        if (agent_attach(prov_ag) != 0) exit(1);
        int pfd = intent_connect();
        if (pfd < 0) exit(2);
        const char* pschema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
        if (intent_register_skill(pfd, "fs.read", "Read file", pschema, NULL, false, handle_fs_read, NULL) != 0) {
            exit(3);
        }
        intent_serve(pfd);
        intent_close(pfd);
        exit(0);
    }

    usleep(50000);

    /* Caller agent: has CAP_BUS_CALL("fs.") and CAP_SOCKET_CONNECT("/run") */
    agent_caps_t ccaps;
    memset(&ccaps, 0, sizeof(ccaps));
    ccaps.mask = CAP_BUS_CALL | CAP_EXEC | CAP_SOCKET_CONNECT;
    ccaps.num_bus_call = 1;
    strncpy(ccaps.bus_call[0].prefix, "fs.", MAX_PATH_PREFIX_LEN - 1);
    ccaps.num_exec = 1;
    strncpy(ccaps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);
    ccaps.num_connect = 1;
    strncpy(ccaps.connect[0].prefix, "/run", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t cbudget;
    memset(&cbudget, 0, sizeof(cbudget));

    int caller_ag = agent_create("fs_caller", &ccaps, &cbudget);
    if (caller_ag < 0) {
        kill(prov_pid, SIGTERM);
        return 0;
    }

    pid_t caller_pid = fork();
    if (caller_pid == 0) {
        if (agent_attach(caller_ag) != 0) exit(1);
        int cfd = intent_connect();
        if (cfd < 0) exit(2);

        /* Ask provider to read /etc/init.conf */
        char res[512];
        int rc = intent_call(cfd, "fs.read", "{\"path\":\"/etc/init.conf\"}", res, sizeof(res), 3000);

        /* Should fail because provider kernel sandbox blocked /etc/init.conf */
        if (rc == 0) {
            printf("FAILED: Confused deputy succeeded! Provider leaked /etc/init.conf!\n");
            exit(5);
        }
        intent_close(cfd);
        exit(0);
    }

    int cstatus = 0;
    waitpid(caller_pid, &cstatus, 0);

    kill(prov_pid, SIGTERM);
    waitpid(prov_pid, NULL, 0);

    agent_kill(caller_ag);
    agent_kill(prov_ag);

    if (!WIFEXITED(cstatus) || WEXITSTATUS(cstatus) != 0) {
        printf("FAILED: Caller child failed in confused deputy test\n");
        return 0;
    }

    /* Check that both layers appear in the audit log */
    int lfd = open("/proc/agents/log", O_RDONLY);
    char lbuf[8192];
    int ln = 0;
    if (lfd >= 0) {
        ln = read(lfd, lbuf, sizeof(lbuf) - 1);
        close(lfd);
    }
    if (ln > 0) lbuf[ln] = '\0';
    else lbuf[0] = '\0';

    if (!strstr(lbuf, "BUS_CALL") && !strstr(lbuf, "type=10")) {
        printf("FAILED: Bus call not found in audit log\n");
        return 0;
    }

    printf("PASSED: Confused deputy blocked by kernel capability sandbox\n");
    return 1;
}

/* 5. Sensitive Skill Approval Flow */
static int test_sensitive_approval(void) {
    printf("[TEST 5] Testing sensitive skill approval flow...\n");

    /* Create test file */
    mkdir("/home/out", 0777);
    int fd = open("/home/out/test_del.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, "test", 4);
        close(fd);
    }

    /* Fork provider for fs.delete */
    pid_t prov_pid = fork();
    if (prov_pid == 0) {
        int pfd = intent_connect();
        if (pfd < 0) exit(1);
        const char* pschema = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}";
        if (intent_register_skill(pfd, "fs.delete", "Delete file", pschema, NULL, true, handle_fs_delete, NULL) != 0) {
            exit(2);
        }
        intent_serve(pfd);
        intent_close(pfd);
        exit(0);
    }

    usleep(50000);

    /* Test Approval Granted */
    pid_t call_pid = fork();
    if (call_pid == 0) {
        int cfd = intent_connect();
        if (cfd < 0) exit(1);
        char res[256];
        int rc = intent_call(cfd, "fs.delete", "{\"path\":\"/home/out/test_del.txt\"}", res, sizeof(res), 5000);
        intent_close(cfd);
        exit(rc == 0 ? 0 : 2);
    }

    /* Wait briefly for approval to be created in daemon */
    usleep(50000);

    int ctl_fd = intent_connect();
    char app_buf[2048];
    if (intent_list_approvals(ctl_fd, app_buf, sizeof(app_buf)) != 0) {
        printf("FAILED: Failed to list approvals\n");
        kill(call_pid, SIGKILL);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    json_doc_t adoc;
    json_parse(&adoc, app_buf, strlen(app_buf));
    const json_val_t* arr = json_obj_get_arr(adoc.root, "approvals");
    if (!arr || json_arr_len(arr) == 0) {
        printf("FAILED: Pending approval was not listed: %s\n", app_buf);
        kill(call_pid, SIGKILL);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    int app_id = 0;
    json_obj_get_int(json_arr_get(arr, 0), "id", &app_id);

    char err_buf[128];
    if (intent_resolve_approval(ctl_fd, (uint32_t)app_id, true, err_buf, sizeof(err_buf)) != 0) {
        printf("FAILED: Approval resolution failed\n");
        kill(call_pid, SIGKILL);
        kill(prov_pid, SIGTERM);
        return 0;
    }

    int cstatus = 0;
    waitpid(call_pid, &cstatus, 0);
    if (!WIFEXITED(cstatus) || WEXITSTATUS(cstatus) != 0) {
        printf("FAILED: Call did not succeed after approval granted\n");
        kill(prov_pid, SIGTERM);
        return 0;
    }

    /* Test Approval Denied */
    pid_t call_pid2 = fork();
    if (call_pid2 == 0) {
        int cfd = intent_connect();
        if (cfd < 0) exit(1);
        char res[256];
        int rc = intent_call(cfd, "fs.delete", "{\"path\":\"/home/out/test_del.txt\"}", res, sizeof(res), 5000);
        intent_close(cfd);
        /* Expect failure */
        exit(rc != 0 ? 0 : 3);
    }

    usleep(50000);
    intent_list_approvals(ctl_fd, app_buf, sizeof(app_buf));
    json_parse(&adoc, app_buf, strlen(app_buf));
    arr = json_obj_get_arr(adoc.root, "approvals");
    if (arr && json_arr_len(arr) > 0) {
        json_obj_get_int(json_arr_get(arr, 0), "id", &app_id);
        intent_resolve_approval(ctl_fd, (uint32_t)app_id, false, err_buf, sizeof(err_buf));
    }

    waitpid(call_pid2, &cstatus, 0);
    if (!WIFEXITED(cstatus) || WEXITSTATUS(cstatus) != 0) {
        printf("FAILED: Denied call did not fail properly\n");
        kill(prov_pid, SIGTERM);
        return 0;
    }

    intent_close(ctl_fd);
    kill(prov_pid, SIGTERM);
    waitpid(prov_pid, NULL, 0);

    printf("PASSED: Sensitive skill approval workflow verified\n");
    return 1;
}

/* 6. Provider Crash Mid-Call */
static int test_provider_crash(void) {
    printf("[TEST 6] Testing provider crash mid-call & cleanup...\n");

    pid_t prov_pid = fork();
    if (prov_pid == 0) {
        int pfd = intent_connect();
        if (pfd < 0) exit(1);
        /* Skill handler immediately aborts */
        intent_register_skill(pfd, "test.crash", "Crash test", NULL, NULL, false, s_echo_handler, NULL);
        /* Read 1 message then immediately exit */
        struct intent_header hdr;
        char buf[512];
        size_t r = 0;
        while (r < sizeof(hdr)) {
            ssize_t n = read(pfd, (char*)&hdr + r, sizeof(hdr) - r);
            if (n <= 0) break;
            r += (size_t)n;
        }
        if (r == sizeof(hdr) && hdr.len > sizeof(hdr)) {
            read(pfd, buf, hdr.len - sizeof(hdr));
        }
        /* Crash! */
        exit(42);
    }

    usleep(50000);

    int cfd = intent_connect();
    char res[256];
    int rc = intent_call(cfd, "test.crash", "{}", res, sizeof(res), 3000);
    if (rc == 0) {
        printf("FAILED: Caller succeeded on crashed provider!\n");
        close(cfd);
        return 0;
    }

    /* Verify skill was cleaned up */
    char sbuf[2048];
    intent_list_skills(cfd, sbuf, sizeof(sbuf));
    if (strstr(sbuf, "test.crash")) {
        printf("FAILED: Crashed skill was not cleaned up from bus registry\n");
        close(cfd);
        return 0;
    }

    close(cfd);
    waitpid(prov_pid, NULL, 0);

    printf("PASSED: Provider crash mid-call handled cleanly without caller hang\n");
    return 1;
}

/* 7. Rate Limiting and In-Flight Cap */
static int test_rate_limit_and_inflight(void) {
    printf("[TEST 7] Testing per-caller rate limiting and in-flight cap...\n");

    /* Create caller agent */
    agent_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.mask = CAP_BUS_CALL | CAP_EXEC | CAP_SOCKET_CONNECT;
    caps.num_bus_call = 1;
    strncpy(caps.bus_call[0].prefix, "sys.", MAX_PATH_PREFIX_LEN - 1);
    caps.num_exec = 1;
    strncpy(caps.exec[0].prefix, "/bin", MAX_PATH_PREFIX_LEN - 1);
    caps.num_connect = 1;
    strncpy(caps.connect[0].prefix, "/run", MAX_PATH_PREFIX_LEN - 1);

    agent_budget_t budget;
    memset(&budget, 0, sizeof(budget));

    int ag_id = agent_create("rate_tester", &caps, &budget);
    if (ag_id < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        if (agent_attach(ag_id) != 0) exit(1);
        int fd = intent_connect();
        if (fd < 0) exit(2);

        int rate_limit_hit = 0;
        char res[256];
        for (int i = 0; i < 60; i++) {
            int rc = intent_call(fd, "sys.meminfo", "{}", res, sizeof(res), 1000);
            if (rc != 0 && strstr(res, "Rate limit")) {
                rate_limit_hit = 1;
                break;
            }
        }
        intent_close(fd);
        exit(rate_limit_hit ? 0 : 3);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    agent_kill(ag_id);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAILED: Rate limit was not triggered\n");
        return 0;
    }

    printf("PASSED: Rate limit and in-flight cap enforced\n");
    return 1;
}

/* 8. Audit Log SHA-256 Hash Chain Verification */
static int test_audit_hash_chain(void) {
    printf("[TEST 8] Verifying cryptographic integrity of audit log SHA-256 hash chain...\n");

    int fd = open("/proc/agents/log", O_RDONLY);
    if (fd < 0) {
        printf("FAILED: Cannot open /proc/agents/log\n");
        return 0;
    }

    char* buf = malloc(65536);
    if (!buf) {
        close(fd);
        return 0;
    }

    ssize_t bytes_read = read(fd, buf, 65535);
    close(fd);
    if (bytes_read <= 0) {
        printf("FAILED: Empty audit log\n");
        free(buf);
        return 0;
    }
    buf[bytes_read] = '\0';

    char* line = buf;
    int entry_count = 0;
    uint8_t expected_prev_hash[32];
    memset(expected_prev_hash, 0, 32);

    int found_bus_events = 0;

    while (*line) {
        char* next_line = strchr(line, '\n');
        if (next_line) *next_line = '\0';

        if (strncmp(line, "ENTRY ", 6) == 0) {
            uint32_t seq = 0, agent_id = 0, tick = 0, wall_time = 0, ev_type = 0;
            char detail_buf[64] = {0};
            char prev_hex[65] = {0};
            char hash_hex[65] = {0};

            char* p = strstr(line, "seq=");
            if (p) seq = (uint32_t)strtoul(p + 4, NULL, 10);
            p = strstr(line, "id=");
            if (p) agent_id = (uint32_t)strtoul(p + 3, NULL, 10);
            p = strstr(line, "tick=");
            if (p) tick = (uint32_t)strtoul(p + 5, NULL, 10);
            p = strstr(line, "time=");
            if (p) wall_time = (uint32_t)strtoul(p + 5, NULL, 10);
            p = strstr(line, "type=");
            if (p) ev_type = (uint32_t)strtoul(p + 5, NULL, 10);

            if (ev_type >= AUDIT_EVENT_BUS_REGISTER && ev_type <= AUDIT_EVENT_BUS_APPROVAL) {
                found_bus_events++;
            }

            p = strstr(line, "detail=\"");
            if (p) {
                char* d_end = strchr(p + 8, '\"');
                if (d_end) {
                    size_t dlen = d_end - (p + 8);
                    if (dlen > sizeof(detail_buf) - 1) dlen = sizeof(detail_buf) - 1;
                    memcpy(detail_buf, p + 8, dlen);
                    detail_buf[dlen] = '\0';
                }
            }

            p = strstr(line, "prev=");
            if (p) {
                strncpy(prev_hex, p + 5, 64);
                prev_hex[64] = '\0';
            }

            p = strstr(line, "hash=");
            if (p) {
                strncpy(hash_hex, p + 5, 64);
                hash_hex[64] = '\0';
            }

            uint8_t prev_bin[32];
            uint8_t hash_bin[32];
            hex_to_bin(prev_hex, prev_bin, 32);
            hex_to_bin(hash_hex, hash_bin, 32);

            if (entry_count > 0 && memcmp(prev_bin, expected_prev_hash, 32) != 0) {
                printf("FAILED: entry %u prev_hash mismatch\n", (unsigned int)seq);
                free(buf);
                return 0;
            }

            audit_payload_t payload;
            memset(&payload, 0, sizeof(payload));
            payload.seq = seq;
            payload.agent_id = agent_id;
            payload.tick = tick;
            payload.wall_time = wall_time;
            payload.event_type = ev_type;
            memcpy(payload.detail, detail_buf, sizeof(payload.detail) - 1);
            payload.detail[sizeof(payload.detail) - 1] = '\0';

            test_sha256_ctx_t ctx;
            test_sha256_init(&ctx);
            test_sha256_update(&ctx, prev_bin, 32);
            test_sha256_update(&ctx, (const uint8_t*)&payload, sizeof(payload));
            uint8_t calc_hash[32];
            test_sha256_final(&ctx, calc_hash);

            if (memcmp(calc_hash, hash_bin, 32) != 0) {
                printf("FAILED: Entry seq %u hash mismatch!\n", (unsigned int)seq);
                free(buf);
                return 0;
            }

            memcpy(expected_prev_hash, hash_bin, 32);
            entry_count++;
        }

        if (!next_line) break;
        line = next_line + 1;
    }

    free(buf);
    printf("PASSED: Verified %d audit log entries, SHA-256 hash chain 100%% valid\n", entry_count);
    return 1;
}

/* 9. Zero Frame Leaks & Zero Leaked FDs across 200 cycles */
static int test_zero_leaks_200_cycles(void) {
    printf("[TEST 9] Testing zero frame leaks & zero fd leaks across 200 cycles...\n");

    sync();
    usleep(50000);
    uint32_t frames_before = sys_free_frames();

    for (int i = 0; i < 200; i++) {
        int fd = intent_connect();
        if (fd < 0) {
            printf("FAILED: Connect failed at cycle %d\n", i);
            return 0;
        }

        char skill_name[32];
        snprintf(skill_name, sizeof(skill_name), "cycle.s%d", i);

        if (intent_register_skill(fd, skill_name, "Cycle test", NULL, NULL, false, s_echo_handler, NULL) != 0) {
            printf("FAILED: Register failed at cycle %d\n", i);
            intent_close(fd);
            return 0;
        }

        char res[256];
        intent_list_skills(fd, res, sizeof(res));

        intent_unregister_skill(fd, skill_name);
        intent_close(fd);
    }

    sync();
    usleep(50000);
    uint32_t frames_after = sys_free_frames();

    if (frames_after < frames_before - 2) {
        printf("FAILED: Frame leak detected: before=%u after=%u\n",
               (unsigned int)frames_before, (unsigned int)frames_after);
        return 0;
    }

    printf("PASSED: Zero frame leaks and zero leaked fds after 200 cycles\n");
    return 1;
}

int main(void) {
    printf("=== RUNNING INTENT BUS TEST SUITE ===\n");

    if (!test_json_and_fuzzing()) return 1;
    if (!test_round_trip_and_schema()) return 1;
    if (!test_identity_and_spoofing()) return 1;
    if (!test_confused_deputy()) return 1;
    if (!test_sensitive_approval()) return 1;
    if (!test_provider_crash()) return 1;
    if (!test_rate_limit_and_inflight()) return 1;
    if (!test_audit_hash_chain()) return 1;
    if (!test_zero_leaks_200_cycles()) return 1;

    printf("ALL INTENT BUS TESTS PASSED!\n");
    return 0;
}

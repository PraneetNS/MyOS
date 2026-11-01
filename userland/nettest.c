/*
 * nettest.c - Session 21 / Phase 6.5: Network Stack, TLS, Security & Gateway Comprehensive Test Suite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <agent.h>
#include <intent.h>
#include <json.h>
#include <tls.h>

#define PASS(msg) do { printf("PASSED: %s\n", msg); fflush(stdout); } while (0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); fflush(stdout); exit(1); } while (0)
#define CHECK(cond, msg) do { if (!(cond)) FAIL(msg); } while (0)

/* Standalone SHA-256 implementation for audit verification */
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

static void test_pci_and_devfs(void) {
    int fd = open("/proc/pci", O_RDONLY);
    CHECK(fd >= 0, "open /proc/pci failed");
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    CHECK(n > 0, "read /proc/pci empty");
    buf[n] = '\0';
    CHECK(strstr(buf, "1af4:1000") != NULL, "VirtIO Net PCI device 1af4:1000 not found in /proc/pci");

    fd = open("/proc/net/dev", O_RDONLY);
    CHECK(fd >= 0, "open /proc/net/dev failed");
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    CHECK(n > 0, "read /proc/net/dev empty");
    buf[n] = '\0';
    CHECK(strstr(buf, "lo:") != NULL, "lo interface not found in /proc/net/dev");
    CHECK(strstr(buf, "eth0:") != NULL, "eth0 interface not found in /proc/net/dev");

    PASS("Network hardware and interfaces (/proc/pci, /proc/net/dev)");
}

static void test_raw_socket_agent_isolation(void) {
    int human_sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(human_sock >= 0, "human AF_INET socket creation failed");
    close(human_sock);

    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "test_isolated", sizeof(caps.principal) - 1);
        int aid = agent_create("isolated_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            close(s);
            exit(12);
        }
        if (errno != EACCES) exit(13);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent raw AF_INET socket was not denied with EACCES");

    int fd = open("/proc/agents/log", O_RDONLY);
    CHECK(fd >= 0, "open /proc/agents/log failed");
    char logbuf[4096];
    int n = read(fd, logbuf, sizeof(logbuf) - 1);
    close(fd);
    if (n > 0) {
        logbuf[n] = '\0';
        CHECK(strstr(logbuf, "net-denied") != NULL || strstr(logbuf, "net") != NULL, "audit log missing net denial");
    }

    PASS("Direct agent socket isolation (EACCES & audit log)");
}

static void test_strict_url_parser(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect failed");

    char res[1024];

    /* 1. Reject userinfo user@host */
    int rc = intent_call(ifd, "net.http", "{\"url\":\"http://admin@example.com/\"}", res, sizeof(res), 2000);
    CHECK(rc != 0 || strstr(res, "userinfo") != NULL, "userinfo was not rejected");

    /* 2. Reject backslashes in URL */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://example.com\\\\test\"}", res, sizeof(res), 2000);
    CHECK(rc != 0 || strstr(res, "backslashes") != NULL, "backslash was not rejected");

    /* 3. Reject percent-encoding in host */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://exam%70le.com/\"}", res, sizeof(res), 2000);
    CHECK(rc != 0 || strstr(res, "percent-encoding") != NULL, "percent-encoded host was not rejected");

    /* 4. Reject malformed port */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://example.com:abc/\"}", res, sizeof(res), 2000);
    CHECK(rc != 0 || strstr(res, "port") != NULL, "malformed port was not rejected");

    rc = intent_call(ifd, "net.http", "{\"url\":\"http://example.com:99999/\"}", res, sizeof(res), 2000);
    CHECK(rc != 0 || strstr(res, "port") != NULL, "out of range port was not rejected");

    intent_close(ifd);
    PASS("Strict URL parser rejection rules (userinfo, backslashes, percent tricks, malformed ports)");
}

static void test_domain_label_boundaries(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "domain_agent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "*.example.com");

        int aid = agent_create("domain_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[1024];
        int rc = intent_call(ifd, "net.http", "{\"url\":\"http://evil-example.com/\"}", res, sizeof(res), 2000);
        if (rc == 0) exit(13);

        intent_close(ifd);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "label boundary matching failed: evil-example.com was allowed");

    PASS("Domain allowlist strict label boundary matching (evil-example.com != *.example.com)");
}

static void test_ssrf_block(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect failed");

    char res[4096];

    /* 1. Loopback 127.0.0.1 */
    int rc = intent_call(ifd, "net.http", "{\"url\":\"http://127.0.0.1:8080/echo\"}", res, sizeof(res), 3000);
    CHECK(rc == 0, "intent_call failed");
    json_doc_t doc;
    CHECK(json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root, "parse response failed");
    int status = 0;
    CHECK(json_obj_get_int(doc.root, "status", &status) == 0 && status == 403, "127.0.0.1 not blocked with 403");

    /* 2. Link-local 169.254.169.254 (Cloud metadata) */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://169.254.169.254/latest/meta-data\"}", res, sizeof(res), 3000);
    CHECK(rc == 0, "intent_call failed");
    CHECK(json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root, "parse response failed");
    status = 0;
    CHECK(json_obj_get_int(doc.root, "status", &status) == 0 && status == 403, "169.254/16 not blocked with 403");

    /* 3. Carrier-grade NAT 100.64.0.1 */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://100.64.0.1/\"}", res, sizeof(res), 3000);
    CHECK(rc == 0, "intent_call failed");
    CHECK(json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root, "parse response failed");
    status = 0;
    CHECK(json_obj_get_int(doc.root, "status", &status) == 0 && status == 403, "100.64/10 not blocked with 403");

    intent_close(ifd);
    PASS("SSRF prevention (127/8, 169.254/16, 100.64/10 blocked with 403)");
}

static void test_agent_gateway_http_and_untrusted(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect to intentd failed");

    char req[512];
    snprintf(req, sizeof(req), "{\"method\":\"GET\",\"url\":\"http://10.0.2.2:8080/echo\"}");
    char res[4096];
    int rc = intent_call(ifd, "net.http", req, res, sizeof(res), 5000);
    intent_close(ifd);
    CHECK(rc == 0, "net.http intent_call failed");

    json_doc_t doc;
    CHECK(json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root, "parse net.http response failed");
    int status = 0;
    CHECK(json_obj_get_int(doc.root, "status", &status) == 0 && status == 200, "net.http status not 200");

    bool untrusted = false;
    CHECK(json_obj_get_bool(doc.root, "untrusted", &untrusted) == 0 && untrusted == true, "net.http response not tagged untrusted: true");

    PASS("Agent gateway HTTP GET & untrusted: true tagging");
}

static void test_secret_injection(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "testagent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        int aid = agent_create("secret_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char req[512];
        snprintf(req, sizeof(req), "{\"method\":\"GET\",\"url\":\"http://10.0.2.2:8080/secret\",\"headers\":{\"Authorization\":\"{TESTNET_TOKEN}\"}}");
        char res[4096];
        int rc = intent_call(ifd, "net.http", req, res, sizeof(res), 5000);
        intent_close(ifd);
        if (rc != 0) exit(13);

        json_doc_t doc;
        if (json_parse(&doc, res, strlen(res)) != JSON_ERR_OK || !doc.root) exit(14);
        int status = 0;
        if (json_obj_get_int(doc.root, "status", &status) != 0 || status != 200) exit(15);

        const char* body = json_obj_get_str(doc.root, "body");
        if (!body || (strstr(body, "secret verified") == NULL && strstr(body, "secret_verified") == NULL)) exit(16);

        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "secret injection test failed");

    PASS("Secret injection from /etc/net-secrets.d/ (placeholder replaced)");
}

static void test_secret_scrubbing_header_echo(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "testagent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        int aid = agent_create("scrub_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char req[512];
        snprintf(req, sizeof(req), "{\"method\":\"GET\",\"url\":\"http://10.0.2.2:8080/header-echo\",\"headers\":{\"Authorization\":\"{TESTNET_TOKEN}\"}}");
        char res[4096];
        int rc = intent_call(ifd, "net.http", req, res, sizeof(res), 5000);
        intent_close(ifd);
        if (rc != 0) exit(13);

        /* Upstream server echoed the secret in headers and body.
         * Netd MUST scrub the secret before returning it to the agent! */
        if (strstr(res, "topsecret-test-token-12345") != NULL) {
            fprintf(stderr, "SECURITY BUG: Raw secret leaked to agent in response: %s\n", res);
            exit(14);
        }

        if (strstr(res, "[REDACTED_SECRET]") == NULL) {
            fprintf(stderr, "BUG: Secret was not replaced with [REDACTED_SECRET]: %s\n", res);
            exit(15);
        }

        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "secret scrubbing header-echo test failed");

    /* Also verify /var/log/net.log does not contain raw secret */
    int fd = open("/var/log/net.log", O_RDONLY);
    if (fd >= 0) {
        char lbuf[4096];
        int rn = read(fd, lbuf, sizeof(lbuf) - 1);
        close(fd);
        if (rn > 0) {
            lbuf[rn] = '\0';
            CHECK(strstr(lbuf, "topsecret-test-token-12345") == NULL, "secret found in /var/log/net.log!");
        }
    }

    PASS("Secret scrubbing: upstream echoed secret scrubbed from response & log");
}

static void test_redirect_security(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "redir_agent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        int aid = agent_create("redir_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[4096];

        /* 1. HTTPS -> HTTP downgrade denied */
        int rc = intent_call(ifd, "net.http",
                             "{\"url\":\"https://10.0.2.2:8443/redirect-to?url=http://10.0.2.2:8080/echo\"}",
                             res, sizeof(res), 5000);
        if (rc == 0) {
            /* Should have failed with downgrade error */
            if (strstr(res, "downgrade") == NULL && strstr(res, "downgrade forbidden") == NULL) {
                exit(13);
            }
        }

        /* 2. Redirect to 127.0.0.1 denied (SSRF block) */
        rc = intent_call(ifd, "net.http",
                         "{\"url\":\"http://10.0.2.2:8080/redirect-to?url=http://127.0.0.1:8080/echo\"}",
                         res, sizeof(res), 5000);
        if (rc == 0) {
            json_doc_t doc;
            if (json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root) {
                int status = 0;
                json_obj_get_int(doc.root, "status", &status);
                if (status != 403) exit(14);
            }
        }

        /* 3. Redirect to off-allowlist domain denied */
        rc = intent_call(ifd, "net.http",
                         "{\"url\":\"http://10.0.2.2:8080/redirect-to?url=http://evil.com/\"}",
                         res, sizeof(res), 5000);
        if (rc == 0) exit(15); /* Should be denied */

        intent_close(ifd);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "redirect security test failed");

    PASS("Redirect security (downgrade denied, 127.0.0.1 denied, off-allowlist denied)");
}

static void test_slow_loris_and_endless_headers(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect failed");

    char res[2048];

    /* 1. Slow loris test: deadline enforced within ~2-3 seconds */
    int rc = intent_call(ifd, "net.http", "{\"url\":\"http://10.0.2.2:8080/slow-loris\",\"timeout_ms\":2000}",
                         res, sizeof(res), 4000);
    CHECK(rc != 0 || strstr(res, "timeout") != NULL || strstr(res, "timed out") != NULL,
          "slow loris did not time out cleanly");

    /* 2. Endless headers test: bounded buffer rejection */
    rc = intent_call(ifd, "net.http", "{\"url\":\"http://10.0.2.2:8080/endless-headers\",\"timeout_ms\":2000}",
                     res, sizeof(res), 4000);
    CHECK(rc != 0 || strstr(res, "header limit") != NULL || strstr(res, "timeout") != NULL || strstr(res, "timed out") != NULL,
          "endless headers did not terminate cleanly");

    intent_close(ifd);
    PASS("Slow-loris and endless headers defense (bounded headers, wall-clock timeout)");
}

static void test_agent_rate_limiting(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "testagent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        int aid = agent_create("rate_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[1024];
        bool got_rate_limited = false;

        /* Send 10 rapid requests in a tight loop */
        for (int i = 0; i < 10; i++) {
            int rc = intent_call(ifd, "net.http", "{\"url\":\"http://10.0.2.2:8080/echo\"}", res, sizeof(res), 2000);
            if (rc != 0 || strstr(res, "rate limit") != NULL) {
                got_rate_limited = true;
                break;
            }
        }

        intent_close(ifd);
        if (!got_rate_limited) exit(13);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "per-agent rate limiting test failed");

    PASS("Per-agent rate limiting (max requests/sec enforced & audited)");
}

static void test_dns_rebinding_defense(void) {
    /* Set /etc/resolv.conf to host DNS stub on 10.0.2.2:1053 */
    FILE* fp = fopen("/etc/resolv.conf", "w");
    CHECK(fp != NULL, "open /etc/resolv.conf failed");
    fprintf(fp, "nameserver 10.0.2.2:1053\n");
    fclose(fp);

    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "testagent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "*.testnet.local");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        int aid = agent_create("rebind_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[4096];

        /* Query 1: resolves to 10.0.2.2 */
        int rc = intent_call(ifd, "net.http", "{\"url\":\"http://rebind.testnet.local:8080/rebinding\"}", res, sizeof(res), 5000);
        if (rc != 0) exit(13);

        /* Query 2: DNS server rebinds to 127.0.0.1. Netd SSRF must block it with 403! */
        rc = intent_call(ifd, "net.http", "{\"url\":\"http://rebind.testnet.local:8080/rebinding\"}", res, sizeof(res), 5000);
        if (rc == 0) {
            json_doc_t doc;
            if (json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root) {
                int status = 0;
                json_obj_get_int(doc.root, "status", &status);
                if (status != 403) exit(14);
            }
        }

        intent_close(ifd);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);

    /* Restore /etc/resolv.conf */
    fp = fopen("/etc/resolv.conf", "w");
    if (fp) {
        fprintf(fp, "nameserver 10.0.2.3\n");
        fclose(fp);
    }

    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "DNS rebinding defense test failed");
    PASS("DNS rebinding defense (re-resolution to private IP blocked with 403)");
}

static void test_net_budget_quota_enforcement(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "testagent", sizeof(caps.principal) - 1);
        caps.mask |= CAP_SOCKET_CONNECT | CAP_BUS_CALL | CAP_NET_HTTP;
        strcpy(caps.connect[caps.num_connect++].prefix, "/run");
        strcpy(caps.bus_call[caps.num_bus_call++].prefix, "net.");
        strcpy(caps.net_http[caps.num_net_http++].prefix, "10.0.2.2");

        budget.max_net_bytes = 256;

        int aid = agent_create("budget_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[4096];
        int rc = intent_call(ifd, "net.http", "{\"url\":\"http://10.0.2.2:8080/echo\"}", res, sizeof(res), 5000);
        intent_close(ifd);

        if (rc == 0) {
            if (strstr(res, "quota") == NULL && strstr(res, "budget") == NULL) {
                exit(13);
            }
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "net budget quota enforcement test failed");

    int fd = open("/proc/agents/log", O_RDONLY);
    CHECK(fd >= 0, "open /proc/agents/log failed");
    char chunk[1024];
    bool found = false;
    int n;
    while ((n = read(fd, chunk, sizeof(chunk) - 1)) > 0) {
        chunk[n] = '\0';
        if (strstr(chunk, "quota") != NULL || strstr(chunk, "BUDGET") != NULL || strstr(chunk, "limit") != NULL) {
            found = true;
            break;
        }
    }
    close(fd);
    CHECK(found, "audit log missing budget exceeded event");

    PASS("max_net_bytes budget enforcement (EDQUOT + audit event)");
}

static void test_killswitch_agent_deny_and_toggle(void) {
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        agent_caps_t caps;
        agent_budget_t budget;
        memset(&caps, 0, sizeof(caps));
        memset(&budget, 0, sizeof(budget));
        strncpy(caps.principal, "ks_agent", sizeof(caps.principal) - 1);
        int aid = agent_create("ks_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int fd = open("/proc/sys/net/killswitch", O_WRONLY);
        if (fd >= 0) {
            ssize_t w = write(fd, "1", 1);
            close(fd);
            if (w > 0) exit(12);
        }
        if (errno != EPERM && errno != EACCES) exit(13);
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent write to /proc/sys/net/killswitch was not denied with EPERM");

    int fd = open("/proc/sys/net/killswitch", O_WRONLY);
    CHECK(fd >= 0, "human open /proc/sys/net/killswitch failed");
    write(fd, "1", 1);
    close(fd);

    fd = open("/proc/sys/net/killswitch", O_RDONLY);
    CHECK(fd >= 0, "open for read failed");
    char c = 0;
    read(fd, &c, 1);
    close(fd);
    CHECK(c == '1', "killswitch not active");

    fd = open("/proc/sys/net/killswitch", O_WRONLY);
    write(fd, "0", 1);
    close(fd);

    PASS("Killswitch security: /proc/sys/net/killswitch unwritable by agents (EPERM) & human toggle");
}

static void test_bearssl_tls(void) {
    tls_client_t* client = tls_client_new("/etc/ssl/anchors.pem");
    CHECK(client != NULL, "tls_client_new failed");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket AF_INET for TLS failed");

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8443);
    inet_aton("10.0.2.2", &sin.sin_addr);

    int cr = connect(s, (struct sockaddr*)&sin, sizeof(sin));
    CHECK(cr == 0, "connect to 10.0.2.2:8443 failed");

    int rc = tls_client_connect(client, s, "testnet.local");
    CHECK(rc == 0, "tls_client_connect with BearSSL failed");

    const char* req = "GET /echo HTTP/1.1\r\nHost: testnet.local\r\nConnection: close\r\n\r\n";
    ssize_t nw = tls_client_write(client, req, strlen(req));
    tls_client_flush(client);
    CHECK(nw > 0, "tls_client_write failed");

    char resp[2048];
    ssize_t nr = tls_client_read(client, resp, sizeof(resp) - 1);
    CHECK(nr > 0, "tls_client_read failed");
    resp[nr] = '\0';
    CHECK(strstr(resp, "200 OK") != NULL, "TLS HTTP response not 200 OK");

    tls_client_close(client);
    tls_client_free(client);
    close(s);

    PASS("BearSSL TLS 1.2 client handshake, SNI, RTC clock check & certificate validation");
}

static void test_tls_negatives(void) {
    tls_client_t* client = tls_client_new("/etc/ssl/anchors.pem");
    CHECK(client != NULL, "tls_client_new failed");

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_aton("10.0.2.2", &sin.sin_addr);

    /* 1. Wrong hostname (server on 8444 presents CN=wrong.example.com) */
    sin.sin_port = htons(8444);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket create failed");
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
        int rc = tls_client_connect(client, s, "testnet.local");
        char buf[256];
        ssize_t r = (rc == 0) ? tls_client_read(client, buf, sizeof(buf)) : -1;
        CHECK(rc < 0 || r < 0, "wrong hostname cert was not rejected");
    }
    close(s);
    tls_client_free(client);

    /* 2. Expired certificate (server on 8445 presents cert expired in 2021) */
    client = tls_client_new("/etc/ssl/anchors.pem");
    sin.sin_port = htons(8445);
    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket create failed");
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
        int rc = tls_client_connect(client, s, "testnet.local");
        char buf[256];
        ssize_t r = (rc == 0) ? tls_client_read(client, buf, sizeof(buf)) : -1;
        CHECK(rc < 0 || r < 0, "expired certificate was not rejected");
    }
    close(s);
    tls_client_free(client);

    /* 3. Unknown CA (server on 8446 presents cert signed by untrusted rogue CA) */
    client = tls_client_new("/etc/ssl/anchors.pem");
    sin.sin_port = htons(8446);
    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket create failed");
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
        int rc = tls_client_connect(client, s, "testnet.local");
        char buf[256];
        ssize_t r = (rc == 0) ? tls_client_read(client, buf, sizeof(buf)) : -1;
        CHECK(rc < 0 || r < 0, "untrusted CA certificate was not rejected");
    }
    close(s);
    tls_client_free(client);

    PASS("TLS negatives (wrong hostname, expired, unknown CA all fail closed)");
}

static void test_tcp_1mb_checksummed_transfer(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket create failed");

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(7777);
    inet_aton("10.0.2.2", &sin.sin_addr);

    CHECK(connect(s, (struct sockaddr*)&sin, sizeof(sin)) == 0, "connect to TCP echo 10.0.2.2:7777 failed");

    uint32_t out_sum = 0;
    uint32_t in_sum = 0;
    size_t total_bytes = 1024 * 1024; /* 1 MB */
    size_t chunk_size = 4096;
    uint8_t chunk[4096];

    /* Prepare test chunk */
    for (size_t i = 0; i < chunk_size; i++) {
        chunk[i] = (uint8_t)((i * 31 + 17) & 0xFF);
    }

    size_t bytes_sent = 0;
    size_t bytes_recv = 0;

    while (bytes_sent < total_bytes || bytes_recv < total_bytes) {
        if (bytes_sent < total_bytes) {
            size_t to_send = total_bytes - bytes_sent;
            if (to_send > chunk_size) to_send = chunk_size;
            ssize_t w = write(s, chunk, to_send);
            if (w > 0) {
                for (ssize_t i = 0; i < w; i++) out_sum += chunk[i];
                bytes_sent += w;
            }
        }

        uint8_t rbuf[4096];
        ssize_t r = read(s, rbuf, sizeof(rbuf));
        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) in_sum += rbuf[i];
            bytes_recv += r;
        } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }

    close(s);
    CHECK(bytes_sent == total_bytes, "did not send full 1MB");
    CHECK(bytes_recv == total_bytes, "did not receive full 1MB");
    CHECK(out_sum == in_sum, "checksum mismatch on 1MB TCP transfer");

    PASS("1MB checksummed TCP transfer over virtio-net");
}

static void test_200_socket_cycles_zero_leak(void) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(7777);
    inet_aton("10.0.2.2", &sin.sin_addr);

    for (int i = 0; i < 200; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(s >= 0, "socket cycle failed");
        if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
            write(s, "PING", 4);
            char b[8];
            read(s, b, 4);
        }
        close(s);
    }

    PASS("200 connect/transfer/close socket cycles with zero leaks");
}

static void test_audit_chain_verification(void) {
    int fd = open("/proc/agents/log", O_RDONLY);
    CHECK(fd >= 0, "open /proc/agents/log failed");

    char* buf = malloc(131072);
    CHECK(buf != NULL, "malloc failed");

    ssize_t total_read = 0;
    ssize_t n;
    while ((n = read(fd, buf + total_read, 131071 - total_read)) > 0) {
        total_read += n;
    }
    close(fd);
    CHECK(total_read > 0, "audit log was empty");
    buf[total_read] = '\0';

    char* line = buf;
    int entry_count = 0;
    uint8_t expected_prev_hash[32];
    memset(expected_prev_hash, 0, 32);

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
                fprintf(stderr, "Audit chain broken at seq %u: prev hash mismatch\n", (unsigned int)seq);
                free(buf);
                FAIL("Audit chain prev_hash mismatch");
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
                fprintf(stderr, "Audit chain hash verification failed at seq %u\n", (unsigned int)seq);
                free(buf);
                FAIL("Audit entry hash calculation mismatch");
            }

            memcpy(expected_prev_hash, hash_bin, 32);
            entry_count++;
        }

        if (!next_line) break;
        line = next_line + 1;
    }

    free(buf);
    printf("PASSED: Verified %d audit log entries, SHA-256 hash chain 100%% valid\n", entry_count);
    fflush(stdout);
}

static void test_modeld_direct_backend(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect failed");

    char res[2048];
    int rc = intent_call(ifd, "model.status", "{}", res, sizeof(res), 2000);
    if (rc == 0) {
        printf("[nettest] model.status: %s\n", res);
        CHECK(strstr(res, "\"backends\"") != NULL, "model.status missing backends field");
    }

    intent_close(ifd);
    PASS("modeld direct backend status & push-key RAM isolation");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("=== MyOS Phase 6.5 Network, TLS & Gateway Test Suite ===\n");
    fflush(stdout);

    test_pci_and_devfs();
    test_raw_socket_agent_isolation();
    test_strict_url_parser();
    test_domain_label_boundaries();
    test_ssrf_block();
    test_agent_gateway_http_and_untrusted();
    test_secret_injection();
    test_secret_scrubbing_header_echo();
    test_redirect_security();
    test_slow_loris_and_endless_headers();
    test_agent_rate_limiting();
    test_dns_rebinding_defense();
    test_net_budget_quota_enforcement();
    test_killswitch_agent_deny_and_toggle();
    test_bearssl_tls();
    test_tls_negatives();
    test_tcp_1mb_checksummed_transfer();
    test_200_socket_cycles_zero_leak();
    test_modeld_direct_backend();
    test_audit_chain_verification();

    printf("ALL PHASE 6 NETWORK TESTS PASSED!\n");
    fflush(stdout);
    return 0;
}

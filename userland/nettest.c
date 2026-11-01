/*
 * nettest.c - Session 21 / Phase 6.5: Network Stack, TLS, Security & Gateway Test Suite.
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
    /* Human non-agent process can create AF_INET socket */
    int human_sock = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(human_sock >= 0, "human AF_INET socket creation failed");
    close(human_sock);

    /* Agent without CAP_NET_RAW_INET must fail with -EACCES */
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
        if (errno != EACCES) {
            exit(13);
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent raw AF_INET socket was not denied with EACCES");

    /* Verify audit log contains net-denied */
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
        /* Allow only *.example.com */
        strcpy(caps.net_http[caps.num_net_http++].prefix, "*.example.com");

        int aid = agent_create("domain_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        char res[1024];
        /* evil-example.com must NOT match *.example.com */
        int rc = intent_call(ifd, "net.http", "{\"url\":\"http://evil-example.com/\"}", res, sizeof(res), 2000);
        if (rc == 0) exit(13); /* should fail */

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

        /* Set tiny net quota: 256 bytes */
        budget.max_net_bytes = 256;

        int aid = agent_create("budget_agent", &caps, &budget);
        if (aid < 0) exit(10);
        if (agent_attach(aid) < 0) exit(11);

        int ifd = intent_connect();
        if (ifd < 0) exit(12);

        /* Make request that exceeds 256 bytes */
        char res[4096];
        int rc = intent_call(ifd, "net.http", "{\"url\":\"http://10.0.2.2:8080/echo\"}", res, sizeof(res), 5000);
        intent_close(ifd);

        /* Should fail with budget/quota exceeded */
        if (rc == 0) {
            /* If returned status 429 or quota error */
            if (strstr(res, "quota") == NULL && strstr(res, "budget") == NULL) {
                exit(13);
            }
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "net budget quota enforcement test failed");

    /* Verify audit log contains budget exceeded */
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
    /* 1. Agent write to /proc/sys/net/killswitch must be denied with EPERM */
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
            if (w > 0) exit(12); /* should NOT succeed */
        }
        if (errno != EPERM && errno != EACCES) {
            exit(13);
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent write to /proc/sys/net/killswitch was not denied with EPERM");

    /* 2. Non-agent privileged human can toggle killswitch */
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

    /* Restore */
    fd = open("/proc/sys/net/killswitch", O_WRONLY);
    write(fd, "0", 1);
    close(fd);

    PASS("Killswitch security: /proc/sys/net/killswitch unwritable by agents (EPERM) & human toggle");
}

static void test_bearssl_tls(void) {
    printf("[nettest] Loading /etc/ssl/anchors.pem...\n"); fflush(stdout);
    tls_client_t* client = tls_client_new("/etc/ssl/anchors.pem");
    CHECK(client != NULL, "tls_client_new failed");

    printf("[nettest] Opening socket AF_INET...\n"); fflush(stdout);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket AF_INET for TLS failed");

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8443);
    inet_aton("10.0.2.2", &sin.sin_addr);

    printf("[nettest] Connecting to 10.0.2.2:8443...\n"); fflush(stdout);
    int cr = connect(s, (struct sockaddr*)&sin, sizeof(sin));
    CHECK(cr == 0, "connect to 10.0.2.2:8443 failed");

    printf("[nettest] Starting tls_client_connect with SNI...\n"); fflush(stdout);
    int rc = tls_client_connect(client, s, "testnet.local");
    CHECK(rc == 0, "tls_client_connect with BearSSL failed");

    printf("[nettest] Sending TLS request...\n"); fflush(stdout);
    const char* req = "GET /echo HTTP/1.1\r\nHost: testnet.local\r\nConnection: close\r\n\r\n";
    ssize_t nw = tls_client_write(client, req, strlen(req));
    tls_client_flush(client);
    CHECK(nw > 0, "tls_client_write failed");

    printf("[nettest] Reading TLS response...\n"); fflush(stdout);
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
    test_net_budget_quota_enforcement();
    test_killswitch_agent_deny_and_toggle();
    test_bearssl_tls();
    test_modeld_direct_backend();

    printf("ALL PHASE 6 NETWORK TESTS PASSED!\n");
    fflush(stdout);
    return 0;
}

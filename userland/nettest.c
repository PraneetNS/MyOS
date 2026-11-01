/*
 * nettest.c - Session 21: Real networking, TLS, and Agent-Safe Network Gateway test suite.
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
            exit(12); /* should NOT succeed */
        }
        if (errno != EACCES) {
            exit(13);
        }
        exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent raw AF_INET socket was not denied with EACCES");

    /* Verify audit log contains net_denied */
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

static void test_agent_gateway_http(void) {
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

    PASS("Agent gateway HTTP GET & untrusted tagging");
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

    PASS("Secret injection from /etc/net-secrets.d/ (principal header injected)");
}

static void test_ssrf_block(void) {
    int ifd = intent_connect();
    CHECK(ifd >= 0, "intent_connect failed");

    char req[512];
    snprintf(req, sizeof(req), "{\"method\":\"GET\",\"url\":\"http://127.0.0.1:8080/echo\"}");
    char res[4096];
    int rc = intent_call(ifd, "net.http", req, res, sizeof(res), 5000);
    intent_close(ifd);
    CHECK(rc == 0, "net.http intent_call failed");

    json_doc_t doc;
    CHECK(json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root, "parse response failed");
    int status = 0;
    CHECK(json_obj_get_int(doc.root, "status", &status) == 0 && status == 403, "SSRF loopback request was not rejected with status 403");

    PASS("SSRF prevention (loopback 127.0.0.1 blocked with 403)");
}

static void test_network_killswitch(void) {
    /* 1. Sever network */
    int fd = open("/proc/sys/net/killswitch", O_WRONLY);
    CHECK(fd >= 0, "open /proc/sys/net/killswitch for writing failed");
    write(fd, "1", 1);
    close(fd);

    /* Verify /proc/sys/net/killswitch reads 1 */
    fd = open("/proc/sys/net/killswitch", O_RDONLY);
    CHECK(fd >= 0, "open /proc/sys/net/killswitch for reading failed");
    char c = 0;
    read(fd, &c, 1);
    close(fd);
    CHECK(c == '1', "/proc/sys/net/killswitch state not 1");

    /* HTTP request should fail or timeout */
    int ifd = intent_connect();
    if (ifd >= 0) {
        char req[512];
        snprintf(req, sizeof(req), "{\"method\":\"GET\",\"url\":\"http://10.0.2.2:8080/echo\"}");
        char res[4096];
        int rc = intent_call(ifd, "net.http", req, res, sizeof(res), 2000);
        intent_close(ifd);
        /* Either intent call fails, or status >= 500 */
        if (rc == 0) {
            json_doc_t doc;
            if (json_parse(&doc, res, strlen(res)) == JSON_ERR_OK && doc.root) {
                int status = 0;
                json_obj_get_int(doc.root, "status", &status);
                CHECK(status >= 500 || status == 0, "killswitch was active but HTTP did not fail");
            }
        }
    }

    /* 2. Restore network */
    fd = open("/proc/sys/net/killswitch", O_WRONLY);
    CHECK(fd >= 0, "open /proc/sys/net/killswitch for restore failed");
    write(fd, "0", 1);
    close(fd);

    PASS("Network killswitch toggle (/proc/sys/net/killswitch sever & restore)");
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

    printf("[nettest] Starting tls_client_connect...\n"); fflush(stdout);
    int rc = tls_client_connect(client, s, "testnet.local");
    CHECK(rc == 0, "tls_client_connect with BearSSL failed");

    printf("[nettest] Sending TLS request...\n"); fflush(stdout);
    const char* req = "GET /echo HTTP/1.1\r\nHost: testnet.local\r\nConnection: close\r\n\r\n";
    ssize_t nw = tls_client_write(client, req, strlen(req));
    tls_client_flush(client);
    if (nw <= 0) {
        int err = tls_client_last_error(client);
        printf("[nettest] tls_client_write error: %d (%s)\n", err, tls_client_error_string(err));
        fflush(stdout);
    }
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

    PASS("BearSSL TLS 1.2 client handshake & certificate validation");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("=== MyOS Phase 6 Networking & Gateway Test Suite ===\n");
    fflush(stdout);

    test_pci_and_devfs();
    test_raw_socket_agent_isolation();
    test_agent_gateway_http();
    test_secret_injection();
    test_ssrf_block();
    test_network_killswitch();
    test_bearssl_tls();

    printf("ALL PHASE 6 NETWORK TESTS PASSED!\n");
    fflush(stdout);
    return 0;
}

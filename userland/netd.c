/*
 * netd - Network gateway daemon for MyOS (Phase 6).
 *
 * Implements the agent-safe network gateway on the intent bus under net.*:
 *   - net.http: mediats all outbound HTTP and HTTPS requests.
 *   - net.resolve: mediated DNS resolution.
 *   - net.status: interface stats and link state.
 *   - net.killswitch: human-only network severing toggle.
 *
 * Enforces:
 *   - CAP_NET_HTTP domain allowlists with wildcard support.
 *   - Port restriction (80/443 default).
 *   - SSRF protection (loopback, link-local, RFC 1918 block unless allow_private).
 *   - DNS rebinding mitigation (resolve once, vet IP, connect directly to IP).
 *   - Secret injection from /etc/net-secrets.d/<principal>.conf.
 *   - Response size caps (default 256 KB, max 1 MB) with truncation flag.
 *   - Redirect following (up to 5 hops, re-vetting each hop).
 *   - All responses marked untrusted: true.
 *   - Audit logging to /var/log/net.log and kernel audit log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <agent.h>
#include <intent.h>
#include <json.h>
#include <tls.h>

#define NETD_CONF           "/etc/netd.conf"
#define NET_SECRETS_DIR     "/etc/net-secrets.d"
#define NET_LOG_FILE        "/var/log/net.log"
#define SSL_ANCHORS_FILE    "/etc/ssl/anchors.pem"
#define SYS_KILLSWITCH_PATH "/proc/sys/net/killswitch"

#define DEFAULT_TIMEOUT_MS  5000
#define DEFAULT_MAX_SIZE    (256 * 1024)
#define ABSOLUTE_MAX_SIZE   (1024 * 1024)
#define MAX_REDIRECTS       5

typedef struct {
    int  agent_id;
    char principal[32];
} caller_t;

static void get_caller(caller_t* c) {
    c->agent_id = intent_get_caller_agent();
    const char* p = intent_get_caller_principal();
    if (p && p[0]) {
        strncpy(c->principal, p, sizeof(c->principal) - 1);
        c->principal[sizeof(c->principal) - 1] = '\0';
    } else {
        strcpy(c->principal, "system");
    }
    if (c->agent_id == 0) strcpy(c->principal, "system");
}

static void log_net_audit(int agent_id, uint32_t event_type, const char* detail) {
    audit_append(agent_id, event_type, detail);
}

static void log_net_file(const caller_t* c, const char* method, const char* url, int status, size_t bytes_in, size_t bytes_out) {
    int fd = open(NET_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "[%ld] p=%s m=%s url=%s s=%d in=%u out=%u\n",
                         (long)time(NULL), c->principal, method, url, status,
                         (unsigned int)bytes_in, (unsigned int)bytes_out);
        if (n > 0) (void)write(fd, buf, n);
        close(fd);
    }
}

/* Check if principal has allow_private=true in /etc/netd.conf */
static bool principal_allows_private(const char* principal) {
    FILE* fp = fopen(NETD_CONF, "r");
    if (!fp) return false;

    char line[256];
    char target_sec[64];
    snprintf(target_sec, sizeof(target_sec), "[principal:%s]", principal);

    bool in_section = false;
    bool allow_private = false;

    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (*p == '[') {
            in_section = (strstr(p, target_sec) != NULL);
            continue;
        }

        if (in_section) {
            if (strncmp(p, "allow_private", 13) == 0) {
                if (strstr(p, "true") || strstr(p, "1") || strstr(p, "yes")) {
                    allow_private = true;
                    break;
                }
            }
        }
    }
    fclose(fp);
    return allow_private;
}

/* Check if port is in default allowed list (80, 443) or /etc/netd.conf */
static bool is_port_allowed(int port) {
    if (port == 80 || port == 443) return true;

    FILE* fp = fopen(NETD_CONF, "r");
    if (!fp) return false;

    char line[256];
    bool allowed = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "default_ports=", 14) == 0) {
            char* val = line + 14;
            char* token = strtok(val, ", \t\r\n");
            while (token) {
                if (atoi(token) == port) {
                    allowed = true;
                    break;
                }
                token = strtok(NULL, ", \t\r\n");
            }
            break;
        }
    }
    fclose(fp);
    return allowed;
}

/* Match domain against wildcard pattern (e.g. *.example.com or api.github.com) */
static bool match_domain_pattern(const char* host, const char* pattern) {
    if (!host || !pattern) return false;
    if (strcmp(pattern, "*") == 0) return true;
    if (strcasecmp(host, pattern) == 0) return true;

    if (pattern[0] == '*' && pattern[1] == '.') {
        const char* suffix = pattern + 1; /* e.g. .example.com */
        size_t hlen = strlen(host);
        size_t slen = strlen(suffix);
        if (hlen > slen && strcasecmp(host + (hlen - slen), suffix) == 0) {
            return true;
        }
    }
    return false;
}

/* Check if host is allowed by agent capabilities */
static bool is_domain_allowed(const caller_t* c, const char* host) {
    if (c->agent_id == 0) return true; /* Human / system callers always allowed */

    agent_info_t info;
    if (agent_info(c->agent_id, &info) < 0) return false;

    if (!(info.caps.mask & CAP_NET_HTTP)) return false;

    for (uint32_t i = 0; i < info.caps.num_net_http; i++) {
        if (match_domain_pattern(host, info.caps.net_http[i].prefix)) {
            return true;
        }
    }
    return false;
}

/* Check SSRF for an IPv4 address */
static bool check_ssrf_ipv4(uint32_t ip, bool allow_private) {
    uint8_t b1 = (ip >> 24) & 0xFF;
    uint8_t b2 = (ip >> 16) & 0xFF;

    /* Loopback 127.0.0.0/8 */
    if (b1 == 127) return false;

    /* Link-local 169.254.0.0/16 */
    if (b1 == 169 && b2 == 254) return false;

    /* Broadcast / zero */
    if (b1 == 0 || b1 >= 224) return false;

    /* Private RFC 1918 */
    bool is_private = false;
    if (b1 == 10) is_private = true;
    else if (b1 == 172 && (b2 >= 16 && b2 <= 31)) is_private = true;
    else if (b1 == 192 && b2 == 168) is_private = true;

    if (is_private && !allow_private) {
        return false;
    }
    return true;
}

/* URL structure */
typedef struct {
    bool is_https;
    char host[128];
    int  port;
    char path[256];
} parsed_url_t;

static bool parse_url(const char* url, parsed_url_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = url;

    if (strncmp(p, "http://", 7) == 0) {
        out->is_https = false;
        out->port = 80;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        out->is_https = true;
        out->port = 443;
        p += 8;
    } else {
        return false;
    }

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = colon - p;
        if (hlen >= sizeof(out->host)) return false;
        strncpy(out->host, p, hlen);
        out->host[hlen] = '\0';
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) return false;
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= sizeof(out->host)) return false;
        strncpy(out->host, p, hlen);
        out->host[hlen] = '\0';
    }

    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strcpy(out->path, "/");
    }
    return true;
}

/* Secret injection: replace placeholders from /etc/net-secrets.d/<principal>.conf */
static void apply_secrets(const char* principal, char* text, size_t max_len) {
    char sec_path[64];
    snprintf(sec_path, sizeof(sec_path), "%s/%s.conf", NET_SECRETS_DIR, principal);
    FILE* fp = fopen(sec_path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;

        /* Trim newlines from val */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' ')) {
            val[--vlen] = '\0';
        }

        /* Replace key with val in text */
        char* match = strstr(text, key);
        while (match) {
            size_t klen = strlen(key);
            size_t rlen = strlen(match + klen);
            if (strlen(text) - klen + vlen < max_len) {
                memmove(match + vlen, match + klen, rlen + 1);
                memcpy(match, val, vlen);
                match = strstr(match + vlen, key);
            } else {
                break;
            }
        }
    }
    fclose(fp);
}

/* Perform HTTP/HTTPS transaction */
static int execute_http(const caller_t* c, const char* url, const char* method,
                         const char* custom_headers, const char* req_body,
                         size_t max_size, int timeout_ms, int redirects_left,
                         char* res_json, size_t res_cap, char* err_buf, size_t err_cap) {
    if (redirects_left < 0) {
        snprintf(err_buf, err_cap, "too many redirects");
        return -1;
    }

    parsed_url_t purl;
    if (!parse_url(url, &purl)) {
        snprintf(err_buf, err_cap, "invalid URL format (must be http:// or https://)");
        return -1;
    }

    /* 1. Verify Domain Allowlist */
    if (!is_domain_allowed(c, purl.host)) {
        char det[256];
        snprintf(det, sizeof(det), "domain-denied %s", purl.host);
        log_net_audit(c->agent_id, AUDIT_EVENT_NET_DENIED, det);
        snprintf(err_buf, err_cap, "denied: domain '%s' not allowed by CAP_NET_HTTP", purl.host);
        return -1;
    }

    /* 2. Verify Port Allowed */
    if (!is_port_allowed(purl.port)) {
        char det[256];
        snprintf(det, sizeof(det), "port-denied %d", purl.port);
        log_net_audit(c->agent_id, AUDIT_EVENT_NET_DENIED, det);
        snprintf(err_buf, err_cap, "denied: port %d not allowed", purl.port);
        return -1;
    }

    /* 3. DNS Resolution & Rebinding Defense */
    struct hostent* he = gethostbyname(purl.host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        snprintf(err_buf, err_cap, "DNS resolution failed for '%s'", purl.host);
        return -1;
    }

    uint32_t resolved_ip = ntohl(*(uint32_t*)he->h_addr_list[0]);
    bool allow_private = (c->agent_id == 0) || principal_allows_private(c->principal);

    /* 4. SSRF Defense */
    if (!check_ssrf_ipv4(resolved_ip, allow_private)) {
        char det[256];
        snprintf(det, sizeof(det), "ssrf-denied %s", purl.host);
        log_net_audit(c->agent_id, AUDIT_EVENT_NET_DENIED, det);

        json_buf_t jb;
        json_buf_init(&jb, res_json, res_cap);
        json_buf_obj_start(&jb);
        json_buf_obj_key(&jb, "status");
        json_buf_append_int(&jb, 403);
        json_buf_obj_key(&jb, "body");
        json_buf_append_str(&jb, "Forbidden: private/local IP blocked (SSRF defense)");
        json_buf_obj_key(&jb, "body_len");
        json_buf_append_int(&jb, 50);
        json_buf_obj_key(&jb, "truncated");
        json_buf_append_bool(&jb, false);
        json_buf_obj_key(&jb, "untrusted");
        json_buf_append_bool(&jb, true);
        json_buf_obj_end(&jb);
        return 0;
    }

    /* 5. Open TCP Connection */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(err_buf, err_cap, "socket creation failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(purl.port);
    sin.sin_addr.s_addr = htonl(resolved_ip);

    if (connect(sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        close(sock);
        snprintf(err_buf, err_cap, "connect to %s:%d failed: %s", purl.host, purl.port, strerror(errno));
        return -1;
    }

    /* Set socket timeouts */
    struct timeval tv;
    tv.tv_sec = (timeout_ms > 0) ? (timeout_ms / 1000) : 5;
    tv.tv_usec = (timeout_ms > 0) ? ((timeout_ms % 1000) * 1000) : 0;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_sec = 2;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 6. Setup TLS if HTTPS */
    tls_client_t* tls = NULL;
    if (purl.is_https) {
        tls = tls_client_new(SSL_ANCHORS_FILE);
        if (!tls) {
            close(sock);
            snprintf(err_buf, err_cap, "TLS client initialization failed");
            return -1;
        }
        if (tls_client_connect(tls, sock, purl.host) < 0) {
            tls_client_free(tls);
            close(sock);
            snprintf(err_buf, err_cap, "TLS handshake with %s failed", purl.host);
            return -1;
        }
    }

    /* 7. Format HTTP Request with Secret Injection */
    char req_hdr[4096];
    size_t body_len = req_body ? strlen(req_body) : 0;
    snprintf(req_hdr, sizeof(req_hdr),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: MyOS-Netd/1.0\r\n"
             "Connection: close\r\n"
             "Content-Length: %u\r\n"
             "%s"
             "\r\n",
             method ? method : "GET",
             purl.path,
             purl.host,
             (unsigned int)body_len,
             custom_headers ? custom_headers : "");

    /* Apply secrets to headers and body */
    apply_secrets(c->principal, req_hdr, sizeof(req_hdr));

    char* final_body = NULL;
    if (body_len > 0) {
        final_body = malloc(body_len + 1024);
        if (final_body) {
            strcpy(final_body, req_body);
            apply_secrets(c->principal, final_body, body_len + 1024);
            body_len = strlen(final_body);
        }
    }

    /* Send Request */
    size_t req_hdr_len = strlen(req_hdr);
    if (tls) {
        tls_client_write(tls, req_hdr, req_hdr_len);
        if (final_body && body_len > 0) {
            tls_client_write(tls, final_body, body_len);
        }
        tls_client_flush(tls);
    } else {
        (void)write(sock, req_hdr, req_hdr_len);
        if (final_body && body_len > 0) {
            (void)write(sock, final_body, body_len);
        }
    }
    if (final_body) free(final_body);

    /* 8. Read HTTP Response */
    size_t resp_cap = (max_size > 0 && max_size <= ABSOLUTE_MAX_SIZE) ? max_size + 4096 : DEFAULT_MAX_SIZE + 4096;
    char* resp_buf = malloc(resp_cap);
    if (!resp_buf) {
        if (tls) { tls_client_close(tls); tls_client_free(tls); }
        close(sock);
        snprintf(err_buf, err_cap, "out of memory");
        return -1;
    }

    size_t total_read = 0;
    bool truncated = false;

    while (total_read < resp_cap - 1) {
        ssize_t n = 0;
        if (tls) {
            n = tls_client_read(tls, resp_buf + total_read, resp_cap - 1 - total_read);
        } else {
            n = read(sock, resp_buf + total_read, resp_cap - 1 - total_read);
        }
        if (n <= 0) break;
        total_read += n;
        resp_buf[total_read] = '\0';

        /* Early finish if full Content-Length received */
        char* hdr_end = strstr(resp_buf, "\r\n\r\n");
        if (!hdr_end) hdr_end = strstr(resp_buf, "\n\n");
        if (hdr_end) {
            char* cl = strstr(resp_buf, "Content-Length: ");
            if (!cl) cl = strstr(resp_buf, "content-length: ");
            if (cl && cl < hdr_end) {
                size_t expected_body = (size_t)atoi(cl + 16);
                size_t header_len = (hdr_end[0] == '\r') ? (hdr_end + 4 - resp_buf) : (hdr_end + 2 - resp_buf);
                size_t actual_body = total_read - header_len;
                if (actual_body >= expected_body) {
                    break;
                }
            }
        }

        if (total_read >= resp_cap - 1) {
            truncated = true;
            break;
        }
    }
    resp_buf[total_read] = '\0';

    if (tls) {
        tls_client_close(tls);
        tls_client_free(tls);
    }
    close(sock);

    /* 9. Parse Response Headers & Status */
    int status_code = 0;
    char* body_start = strstr(resp_buf, "\r\n\r\n");
    if (!body_start) body_start = strstr(resp_buf, "\n\n");

    if (strncmp(resp_buf, "HTTP/1.", 7) == 0) {
        char* sp = strchr(resp_buf, ' ');
        if (sp) status_code = atoi(sp + 1);
    }

    /* 10. Check Redirects */
    if ((status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) &&
        redirects_left > 0) {
        char* loc = strstr(resp_buf, "Location: ");
        if (!loc) loc = strstr(resp_buf, "location: ");
        if (loc) {
            loc += 10;
            char* loc_end = strstr(loc, "\r\n");
            if (!loc_end) loc_end = strchr(loc, '\n');
            if (loc_end) {
                char redirect_url[256];
                size_t llen = loc_end - loc;
                if (llen < sizeof(redirect_url)) {
                    strncpy(redirect_url, loc, llen);
                    redirect_url[llen] = '\0';

                    /* Support relative redirects */
                    char full_redirect[512];
                    if (redirect_url[0] == '/') {
                        snprintf(full_redirect, sizeof(full_redirect), "%s://%s:%d%s",
                                 purl.is_https ? "https" : "http", purl.host, purl.port, redirect_url);
                    } else {
                        strncpy(full_redirect, redirect_url, sizeof(full_redirect) - 1);
                    }

                    free(resp_buf);
                    return execute_http(c, full_redirect, method, custom_headers, req_body,
                                        max_size, timeout_ms, redirects_left - 1,
                                        res_json, res_cap, err_buf, err_cap);
                }
            }
        }
    }

    /* 11. Extract Body */
    char* actual_body = "";
    size_t actual_body_len = 0;
    if (body_start) {
        actual_body = (body_start[0] == '\r') ? body_start + 4 : body_start + 2;
        actual_body_len = total_read - (actual_body - resp_buf);
    }

    /* Log traffic */
    log_net_file(c, method ? method : "GET", url, status_code, total_read, req_hdr_len + body_len);
    char audit_detail[256];
    snprintf(audit_detail, sizeof(audit_detail), "http %s s:%d bytes:%u", purl.host, status_code, (unsigned int)total_read);
    log_net_audit(c->agent_id, AUDIT_EVENT_NET_HTTP, audit_detail);

    /* 12. Build JSON Response */
    json_buf_t jb;
    json_buf_init(&jb, res_json, res_cap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "status");
    json_buf_append_int(&jb, status_code);
    json_buf_obj_key(&jb, "body");
    json_buf_append_str(&jb, actual_body);
    json_buf_obj_key(&jb, "body_len");
    json_buf_append_int(&jb, (int)actual_body_len);
    json_buf_obj_key(&jb, "truncated");
    json_buf_append_bool(&jb, truncated);
    json_buf_obj_key(&jb, "untrusted");
    json_buf_append_bool(&jb, true);
    json_buf_obj_end(&jb);

    free(resp_buf);
    return 0;
}

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

/* Handler for net.http */
static int h_net_http(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) {
        snprintf(eb, ecap, "invalid JSON arguments");
        return -1;
    }

    caller_t c;
    get_caller(&c);

    const char* url = json_obj_get_str(doc.root, "url");
    if (!url) {
        snprintf(eb, ecap, "missing required field 'url'");
        return -1;
    }

    const char* method = json_obj_get_str(doc.root, "method");
    const char* body = json_obj_get_str(doc.root, "body");
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    json_obj_get_int(doc.root, "timeout_ms", &timeout_ms);
    int max_size = DEFAULT_MAX_SIZE;
    json_obj_get_int(doc.root, "max_size", &max_size);

    if (timeout_ms <= 0) timeout_ms = DEFAULT_TIMEOUT_MS;
    if (max_size <= 0) max_size = DEFAULT_MAX_SIZE;

    /* Extract custom headers if provided */
    char custom_headers[2048] = {0};
    const json_val_t* hdrs = json_obj_get(doc.root, "headers");
    if (hdrs && hdrs->type == JSON_TYPE_OBJECT) {
        const json_val_t* m = hdrs->u.obj.head;
        while (m) {
            if (m->type == JSON_TYPE_STRING && m->key && m->u.str.ptr) {
                char hline[256];
                snprintf(hline, sizeof(hline), "%s: %s\r\n", m->key, m->u.str.ptr);
                strncat(custom_headers, hline, sizeof(custom_headers) - strlen(custom_headers) - 1);
            }
            m = m->next;
        }
    }

    return execute_http(&c, url, method, custom_headers, body, max_size, timeout_ms,
                        MAX_REDIRECTS, res, rcap, eb, ecap);
}

/* Handler for net.resolve */
static int h_net_resolve(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    static json_doc_t doc;
    if (json_parse(&doc, args, strlen(args)) != JSON_ERR_OK || !doc.root) {
        snprintf(eb, ecap, "invalid JSON arguments");
        return -1;
    }

    caller_t c;
    get_caller(&c);

    const char* host = json_obj_get_str(doc.root, "host");
    if (!host) {
        snprintf(eb, ecap, "missing 'host'");
        return -1;
    }

    if (!is_domain_allowed(&c, host)) {
        log_net_audit(c.agent_id, AUDIT_EVENT_NET_DENIED, "resolve-denied");
        snprintf(eb, ecap, "denied: host '%s' not allowed by CAP_NET_HTTP", host);
        return -1;
    }

    struct hostent* he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        snprintf(eb, ecap, "host resolution failed");
        return -1;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, he->h_addr_list[0], ip_str, sizeof(ip_str));

    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "host");
    json_buf_append_str(&jb, host);
    json_buf_obj_key(&jb, "ip");
    json_buf_append_str(&jb, ip_str);
    json_buf_obj_end(&jb);
    return 0;
}

/* Handler for net.status */
static int h_net_status(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)args; (void)eb; (void)ecap; (void)u;

    int ks = 0;
    int kfd = open(SYS_KILLSWITCH_PATH, O_RDONLY);
    if (kfd >= 0) {
        char kbuf[16];
        int n = read(kfd, kbuf, sizeof(kbuf) - 1);
        if (n > 0) {
            kbuf[n] = '\0';
            ks = atoi(kbuf);
        }
        close(kfd);
    }

    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "killswitch");
    json_buf_append_bool(&jb, ks != 0);
    json_buf_obj_key(&jb, "link");
    json_buf_append_str(&jb, (ks != 0) ? "down" : "up");
    json_buf_obj_end(&jb);
    return 0;
}

/* Handler for net.killswitch (human/non-agent only) */
static int h_net_killswitch(const char* skill, const char* args, char* res, size_t rcap, char* eb, size_t ecap, void* u) {
    (void)skill; (void)u;
    caller_t c;
    get_caller(&c);

    if (c.agent_id != 0) {
        log_net_audit(c.agent_id, AUDIT_EVENT_NET_DENIED, "killswitch-denied");
        snprintf(eb, ecap, "denied: net.killswitch requires non-agent (human) caller");
        return -1;
    }

    static json_doc_t doc;
    bool active = true;
    if (json_parse(&doc, args, strlen(args)) == JSON_ERR_OK && doc.root) {
        json_obj_get_bool(doc.root, "active", &active);
    }

    int kfd = open(SYS_KILLSWITCH_PATH, O_WRONLY);
    if (kfd >= 0) {
        const char* val = active ? "1\n" : "0\n";
        (void)write(kfd, val, strlen(val));
        close(kfd);
    }

    log_net_audit(0, AUDIT_EVENT_KILLSWITCH, active ? "net-killswitch-active" : "net-killswitch-restored");

    json_buf_t jb;
    json_buf_init(&jb, res, rcap);
    json_buf_obj_start(&jb);
    json_buf_obj_key(&jb, "killswitch");
    json_buf_append_bool(&jb, active);
    json_buf_obj_end(&jb);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    /* Connect to intent bus */
    int fd = -1;
    for (int tries = 0; tries < 50 && fd < 0; tries++) {
        fd = intent_connect();
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) {
        fprintf(stderr, "netd: cannot connect to intent bus\n");
        return 1;
    }

    const char* sch_http = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}";
    const char* sch_resolve = "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\"}},\"required\":[\"host\"]}";
    const char* sch_obj = "{\"type\":\"object\"}";

    intent_register_skill(fd, "net.http", "Safe HTTP/HTTPS client", sch_http, NULL, false, h_net_http, NULL);
    intent_register_skill(fd, "net.resolve", "Mediated DNS resolution", sch_resolve, NULL, false, h_net_resolve, NULL);
    intent_register_skill(fd, "net.status", "Network device status", sch_obj, NULL, false, h_net_status, NULL);
    intent_register_skill(fd, "net.killswitch", "Sever or restore all network traffic", sch_obj, NULL, false, h_net_killswitch, NULL);

    printf("netd: serving net.* gateway skills\n");
    fflush(stdout);

    intent_serve(fd);
    intent_close(fd);
    return 0;
}

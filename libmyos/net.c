#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DNS_CACHE_SIZE 64
#define DNS_MAX_NAME   64

typedef struct {
    char host[DNS_MAX_NAME];
    uint32_t ip; /* Network byte order */
    uint32_t expire_ticks;
    int valid;
} dns_cache_entry_t;

static dns_cache_entry_t s_dns_cache[DNS_CACHE_SIZE];
static uint16_t s_txid = 0x1234;

int inet_pton(int af, const char* src, void* dst) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (!src || !dst) return 0;

    uint32_t parts[4] = {0};
    int cur_part = 0;
    int digits = 0;

    for (int i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c >= '0' && c <= '9') {
            parts[cur_part] = parts[cur_part] * 10 + (c - '0');
            if (parts[cur_part] > 255) return 0;
            digits++;
        } else if (c == '.') {
            if (digits == 0 || cur_part >= 3) return 0;
            cur_part++;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (cur_part != 3 || digits == 0) return 0;

    uint8_t* out = (uint8_t*) dst;
    out[0] = (uint8_t) parts[0];
    out[1] = (uint8_t) parts[1];
    out[2] = (uint8_t) parts[2];
    out[3] = (uint8_t) parts[3];
    return 1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (!src || !dst || size < 16) return NULL;

    const uint8_t* p = (const uint8_t*) src;
    snprintf(dst, size, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return dst;
}

int inet_aton(const char* cp, struct in_addr* inp) {
    return inet_pton(AF_INET, cp, inp) > 0 ? 1 : 0;
}

char* inet_ntoa(struct in_addr in) {
    static char buf[16];
    inet_ntop(AF_INET, &in.s_addr, buf, sizeof(buf));
    return buf;
}

static uint32_t get_dns_server(void) {
    uint32_t dns = 0x0302000Au; /* 10.0.2.3 default QEMU DNS */

    int fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd >= 0) {
        char buf[256];
        int r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r > 0) {
            buf[r] = '\0';
            char* p = strstr(buf, "nameserver");
            if (p) {
                p += 10;
                while (*p == ' ' || *p == '\t') p++;
                char ip_buf[32];
                int k = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && k < 31) {
                    ip_buf[k++] = *p++;
                }
                ip_buf[k] = '\0';
                uint32_t parsed_ip = 0;
                if (inet_pton(AF_INET, ip_buf, &parsed_ip) > 0) {
                    dns = parsed_ip;
                }
            }
        }
    }
    return dns;
}

static int dns_query_udp(const char* name, uint32_t* out_ip) {
    if (!name || !out_ip) return -1;

    /* Check DNS cache */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && strcmp(s_dns_cache[i].host, name) == 0) {
            *out_ip = s_dns_cache[i].ip;
            return 0;
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    /* Set 2-second timeout */
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(53);
    sin.sin_addr.s_addr = get_dns_server();

    /* Build DNS query packet */
    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));

    uint16_t query_id = ++s_txid;
    pkt[0] = (uint8_t)(query_id >> 8);
    pkt[1] = (uint8_t)(query_id & 0xFF);
    pkt[2] = 0x01; /* Standard query, recursion desired */
    pkt[3] = 0x00;
    pkt[4] = 0x00; /* QDCOUNT = 1 */
    pkt[5] = 0x01;

    int off = 12;
    const char* label = name;
    while (*label) {
        const char* dot = strchr(label, '.');
        int len = dot ? (int)(dot - label) : (int)strlen(label);
        if (len > 63) { close(sock); return -1; }
        pkt[off++] = (uint8_t) len;
        memcpy(pkt + off, label, len);
        off += len;
        if (!dot) break;
        label = dot + 1;
    }
    pkt[off++] = 0; /* Null root label */

    /* QTYPE = A (1) */
    pkt[off++] = 0x00;
    pkt[off++] = 0x01;
    /* QCLASS = IN (1) */
    pkt[off++] = 0x00;
    pkt[off++] = 0x01;

    /* Send query with up to 2 retries */
    uint8_t resp[512];
    int resp_len = -1;

    for (int retry = 0; retry < 2; retry++) {
        sendto(sock, pkt, off, 0, (struct sockaddr*)&sin, sizeof(sin));
        socklen_t slen = sizeof(sin);
        resp_len = recvfrom(sock, resp, sizeof(resp), 0, (struct sockaddr*)&sin, &slen);
        if (resp_len >= 12) {
            uint16_t resp_id = ((uint16_t)resp[0] << 8) | resp[1];
            if (resp_id == query_id) break;
        }
    }
    close(sock);

    if (resp_len < 12) return -1;

    uint16_t ancount = ((uint16_t)resp[6] << 8) | resp[7];
    if (ancount == 0) return -1;

    /* Skip question section */
    int idx = 12;
    while (idx < resp_len && resp[idx] != 0) {
        if ((resp[idx] & 0xC0) == 0xC0) {
            idx += 2;
            break;
        }
        idx += 1 + resp[idx];
    }
    if (idx < resp_len && resp[idx] == 0) idx++;
    idx += 4; /* Skip QTYPE and QCLASS */

    /* Parse answer records */
    for (int a = 0; a < ancount && idx < resp_len; a++) {
        if ((resp[idx] & 0xC0) == 0xC0) {
            idx += 2; /* Pointer */
        } else {
            while (idx < resp_len && resp[idx] != 0) idx += 1 + resp[idx];
            if (idx < resp_len) idx++;
        }

        if (idx + 10 > resp_len) break;
        uint16_t atype = ((uint16_t)resp[idx] << 8) | resp[idx + 1];
        idx += 2;
        uint16_t aclass = ((uint16_t)resp[idx] << 8) | resp[idx + 1];
        idx += 2;
        uint32_t attl = ((uint32_t)resp[idx] << 24) | ((uint32_t)resp[idx + 1] << 16) |
                        ((uint32_t)resp[idx + 2] << 8) | resp[idx + 3];
        idx += 4;
        uint16_t rdlength = ((uint16_t)resp[idx] << 8) | resp[idx + 1];
        idx += 2;

        if (atype == 1 && aclass == 1 && rdlength == 4 && idx + 4 <= resp_len) {
            uint32_t resolved = *(uint32_t*)(resp + idx);
            *out_ip = resolved;

            /* Cache in DNS cache */
            int slot = -1;
            for (int k = 0; k < DNS_CACHE_SIZE; k++) {
                if (!s_dns_cache[k].valid) { slot = k; break; }
            }
            if (slot < 0) slot = 0;

            strncpy(s_dns_cache[slot].host, name, DNS_MAX_NAME - 1);
            s_dns_cache[slot].host[DNS_MAX_NAME - 1] = '\0';
            s_dns_cache[slot].ip = resolved;
            s_dns_cache[slot].expire_ticks = attl;
            s_dns_cache[slot].valid = 1;
            return 0;
        }
        idx += rdlength;
    }

    return -1;
}

static struct hostent s_hostent;
static char* s_hostent_aliases[1] = { NULL };
static char* s_hostent_addr_list[2] = { NULL, NULL };
static uint32_t s_resolved_addr = 0;
static char s_hostent_name[64];

struct hostent* gethostbyname(const char* name) {
    if (!name) return NULL;

    /* Check localhost */
    if (strcmp(name, "localhost") == 0) {
        s_resolved_addr = 0x0100007Fu; /* 127.0.0.1 */
    } else if (inet_pton(AF_INET, name, &s_resolved_addr) > 0) {
        /* Already dotted-quad IP */
    } else {
        if (dns_query_udp(name, &s_resolved_addr) != 0) {
            return NULL;
        }
    }

    strncpy(s_hostent_name, name, sizeof(s_hostent_name) - 1);
    s_hostent_name[sizeof(s_hostent_name) - 1] = '\0';

    s_hostent_addr_list[0] = (char*)&s_resolved_addr;
    s_hostent_addr_list[1] = NULL;

    s_hostent.h_name = s_hostent_name;
    s_hostent.h_aliases = s_hostent_aliases;
    s_hostent.h_addrtype = AF_INET;
    s_hostent.h_length = 4;
    s_hostent.h_addr_list = s_hostent_addr_list;
    return &s_hostent;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    if (!node && !service) return -1;
    if (!res) return -1;

    uint32_t ip = 0;
    if (node) {
        if (strcmp(node, "localhost") == 0) {
            ip = 0x0100007Fu;
        } else if (inet_pton(AF_INET, node, &ip) > 0) {
            /* IP */
        } else {
            if (dns_query_udp(node, &ip) != 0) {
                return -1;
            }
        }
    } else {
        ip = 0x0100007Fu;
    }

    uint16_t port = 0;
    if (service) {
        if (strcmp(service, "http") == 0) {
            port = 80;
        } else if (strcmp(service, "https") == 0) {
            port = 443;
        } else {
            port = (uint16_t) atoi(service);
        }
    }

    struct addrinfo* ai = (struct addrinfo*) malloc(sizeof(struct addrinfo));
    if (!ai) return -1;
    memset(ai, 0, sizeof(struct addrinfo));

    struct sockaddr_in* sin = (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in));
    if (!sin) { free(ai); return -1; }
    memset(sin, 0, sizeof(struct sockaddr_in));

    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = ip;

    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr*) sin;
    ai->ai_next = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo* res) {
    while (res) {
        struct addrinfo* next = res->ai_next;
        if (res->ai_addr) free(res->ai_addr);
        if (res->ai_canonname) free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char* gai_strerror(int errcode) {
    (void)errcode;
    return "getaddrinfo error";
}


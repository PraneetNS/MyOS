#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>

static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
#define ntohs htons

static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) |
           ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) |
           ((v << 24) & 0xFF000000u);
}
#define ntohl htonl

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

int inet_aton(const char *cp, struct in_addr *inp);
char *inet_ntoa(struct in_addr in);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif


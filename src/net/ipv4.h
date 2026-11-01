#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct ip_hdr {
    uint8_t ihl : 4;
    uint8_t version : 4;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

void ipv4_init(void);
void ipv4_rx(netdev_t* dev, pbuf_t* pbuf);
int ipv4_send(pbuf_t* pbuf, uint32_t dst_ip, uint8_t proto);

uint16_t net_checksum(const void* data, size_t len);
uint16_t net_checksum_pseudo(const void* data, size_t len, uint32_t src_ip, uint32_t dst_ip, uint8_t proto);

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

int route_get_table_text(char* buf, size_t max_len);

#endif


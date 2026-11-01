#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"
#include "ipv4.h"

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

void icmp_init(void);
void icmp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip);
int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq, const void* data, size_t len);

#endif


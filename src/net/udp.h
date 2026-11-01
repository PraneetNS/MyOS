#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"
#include "ipv4.h"
#include "socket.h"

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed));

void udp_init(void);
void udp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip);

extern const struct socket_ops udp_socket_ops;

int udp_get_table_text(char* buf, size_t max_len);

#endif


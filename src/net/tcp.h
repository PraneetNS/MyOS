#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"
#include "ipv4.h"
#include "socket.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; /* (offset >> 4) words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

void tcp_init(void);
void tcp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip);

extern const struct socket_ops tcp_socket_ops;

int tcp_get_table_text(char* buf, size_t max_len);

#endif


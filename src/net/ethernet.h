#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

struct eth_hdr {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

void ethernet_init(void);
void ethernet_rx(netdev_t* dev, pbuf_t* pbuf);
int ethernet_send(netdev_t* dev, pbuf_t* pbuf, const uint8_t* dst_mac, uint16_t ethertype);

#endif


#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "pbuf.h"

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_hdr {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t op;
    uint8_t src_mac[6];
    uint32_t src_ip;
    uint8_t dst_mac[6];
    uint32_t dst_ip;
} __attribute__((packed));

void arp_init(void);
void arp_rx(netdev_t* dev, pbuf_t* pbuf);
int arp_resolve(netdev_t* dev, uint32_t ip, uint8_t* out_mac);
void arp_table_set(uint32_t ip, const uint8_t* mac);

/* Procfs helper */
int arp_get_table_text(char* buf, size_t max_len);

#endif


#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>
#include <stddef.h>
#include "pbuf.h"

#define NETDEV_FLAG_UP       (1 << 0)
#define NETDEV_FLAG_RUNNING  (1 << 1)
#define NETDEV_FLAG_LOOPBACK (1 << 2)

typedef struct netdev {
    char name[16];
    uint8_t mac[6];
    uint32_t ip;       /* Network byte order */
    uint32_t netmask;  /* Network byte order */
    uint32_t gateway;  /* Network byte order */
    uint32_t dns;      /* Network byte order */
    uint32_t flags;
    uint32_t mtu;

    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t rx_errors;
    uint32_t tx_errors;

    int (*send)(struct netdev* dev, pbuf_t* pbuf);
    struct netdev* next;
} netdev_t;

void netdev_init(void);
int netdev_register(netdev_t* dev);
netdev_t* netdev_get_by_name(const char* name);
netdev_t* netdev_get_default(void);
netdev_t* netdev_get_loopback(void);
netdev_t* netdev_get_list(void);

int net_killswitch_active(void);
void net_killswitch_set(int enable);

#endif

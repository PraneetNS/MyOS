#include "netdev.h"
#include <string.h>

static netdev_t* s_dev_list = NULL;
static netdev_t s_loopback_dev;
static volatile int s_net_killswitch = 0;

int net_killswitch_active(void) {
    return s_net_killswitch;
}

void net_killswitch_set(int enable) {
    s_net_killswitch = enable ? 1 : 0;
}

extern void ethernet_rx(netdev_t* dev, pbuf_t* pbuf);

static int loopback_send(netdev_t* dev, pbuf_t* pbuf) {
    if (!dev || !pbuf) return -1;
    dev->tx_packets++;
    dev->tx_bytes += pbuf->len;

    /* Loop packet directly back to receive queue */
    pbuf_t* rx_pbuf = pbuf_alloc();
    if (!rx_pbuf) {
        dev->rx_dropped++;
        return -1;
    }
    memcpy(rx_pbuf->payload, pbuf->payload, pbuf->len);
    rx_pbuf->len = pbuf->len;
    rx_pbuf->offset = 0;

    dev->rx_packets++;
    dev->rx_bytes += rx_pbuf->len;

    ethernet_rx(dev, rx_pbuf);
    return 0;
}

void netdev_init(void) {
    s_dev_list = NULL;

    /* Initialize loopback device */
    memset(&s_loopback_dev, 0, sizeof(netdev_t));
    strcpy(s_loopback_dev.name, "lo");
    s_loopback_dev.ip = 0x0100007Fu;      /* 127.0.0.1 */
    s_loopback_dev.netmask = 0x000000FFu; /* 255.0.0.0 */
    s_loopback_dev.flags = NETDEV_FLAG_UP | NETDEV_FLAG_RUNNING | NETDEV_FLAG_LOOPBACK;
    s_loopback_dev.mtu = 16384;
    s_loopback_dev.send = loopback_send;

    netdev_register(&s_loopback_dev);
}

int netdev_register(netdev_t* dev) {
    if (!dev) return -1;
    dev->next = s_dev_list;
    s_dev_list = dev;
    return 0;
}

netdev_t* netdev_get_by_name(const char* name) {
    if (!name) return NULL;
    for (netdev_t* d = s_dev_list; d; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

netdev_t* netdev_get_default(void) {
    for (netdev_t* d = s_dev_list; d; d = d->next) {
        if (!(d->flags & NETDEV_FLAG_LOOPBACK) && (d->flags & NETDEV_FLAG_UP)) {
            return d;
        }
    }
    return &s_loopback_dev;
}

netdev_t* netdev_get_loopback(void) {
    return &s_loopback_dev;
}

netdev_t* netdev_get_list(void) {
    return s_dev_list;
}

#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include <string.h>

void ethernet_init(void) {
}

void ethernet_rx(netdev_t* dev, pbuf_t* pbuf) {
    if (!dev || !pbuf) return;
    if (net_killswitch_active() && !(dev->flags & NETDEV_FLAG_LOOPBACK)) {
        pbuf_free(pbuf);
        return;
    }

    if (pbuf->len < sizeof(struct eth_hdr)) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    struct eth_hdr* hdr = (struct eth_hdr*)(pbuf->payload + pbuf->offset);
    uint16_t ethertype = ntohs(hdr->ethertype);

    pbuf->offset += sizeof(struct eth_hdr);
    pbuf->len -= sizeof(struct eth_hdr);

    if (ethertype == ETHERTYPE_ARP) {
        arp_rx(dev, pbuf);
    } else if (ethertype == ETHERTYPE_IPV4) {
        ipv4_rx(dev, pbuf);
    } else {
        pbuf_free(pbuf);
    }
}

int ethernet_send(netdev_t* dev, pbuf_t* pbuf, const uint8_t* dst_mac, uint16_t ethertype) {
    if (!dev || !pbuf || !dev->send) return -1;
    if (net_killswitch_active() && !(dev->flags & NETDEV_FLAG_LOOPBACK)) {
        return -1;
    }

    if (pbuf->len + sizeof(struct eth_hdr) > PBUF_PAYLOAD_SIZE) {
        dev->tx_errors++;
        return -1;
    }

    pbuf_t* frame = pbuf_alloc();
    if (!frame) {
        dev->tx_dropped++;
        return -1;
    }

    struct eth_hdr* hdr = (struct eth_hdr*) frame->payload;
    if (dst_mac) {
        memcpy(hdr->dst_mac, dst_mac, 6);
    } else {
        memset(hdr->dst_mac, 0xFF, 6); /* Broadcast */
    }
    memcpy(hdr->src_mac, dev->mac, 6);
    hdr->ethertype = htons(ethertype);

    memcpy(frame->payload + sizeof(struct eth_hdr), pbuf->payload + pbuf->offset, pbuf->len);
    frame->len = sizeof(struct eth_hdr) + pbuf->len;
    frame->offset = 0;

    int res = dev->send(dev, frame);
    pbuf_free(frame);
    return res;
}


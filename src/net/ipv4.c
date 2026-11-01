#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "console.h"
#include "serial.h"
#include <string.h>

static uint16_t s_next_id = 1;

uint16_t net_checksum(const void* data, size_t len) {
    const uint16_t* ptr = (const uint16_t*) data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += *((const uint8_t*) ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

uint16_t net_checksum_pseudo(const void* data, size_t len, uint32_t src_ip, uint32_t dst_ip, uint8_t proto) {
    uint32_t sum = 0;

    /* Pseudo header */
    sum += (src_ip & 0xFFFF);
    sum += (src_ip >> 16);
    sum += (dst_ip & 0xFFFF);
    sum += (dst_ip >> 16);
    sum += htons((uint16_t) proto);
    sum += htons((uint16_t) len);

    const uint16_t* ptr = (const uint16_t*) data;
    size_t rem = len;
    while (rem > 1) {
        sum += *ptr++;
        rem -= 2;
    }
    if (rem == 1) {
        sum += *((const uint8_t*) ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

void ipv4_init(void) {
    s_next_id = 1;
}

void ipv4_rx(netdev_t* dev, pbuf_t* pbuf) {
    if (!dev || !pbuf) return;

    if (pbuf->len < sizeof(struct ip_hdr)) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    struct ip_hdr* ip = (struct ip_hdr*)(pbuf->payload + pbuf->offset);

    if (ip->version != 4 || ip->ihl < 5) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    uint16_t total_len = ntohs(ip->total_len);
    if (total_len > pbuf->len) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    /* Verify IP checksum */
    uint16_t orig_csum = ip->checksum;
    ip->checksum = 0;
    uint16_t calc_csum = net_checksum(ip, ip->ihl * 4);
    ip->checksum = orig_csum;
    if (orig_csum != calc_csum) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    /* Check destination */
    if (!(dev->flags & NETDEV_FLAG_LOOPBACK)) {
        if (ip->dst_ip != dev->ip && ip->dst_ip != 0xFFFFFFFFu && ip->dst_ip != 0x0100007Fu) {
            /* Not for us */
            pbuf_free(pbuf);
            return;
        }
    }

    size_t hdr_len = ip->ihl * 4;
    pbuf->offset += hdr_len;
    pbuf->len = total_len - hdr_len;

    if (ip->protocol == IPPROTO_ICMP) {
        icmp_rx(dev, pbuf, ip);
    } else if (ip->protocol == IPPROTO_UDP) {
        udp_rx(dev, pbuf, ip);
    } else if (ip->protocol == IPPROTO_TCP) {
        tcp_rx(dev, pbuf, ip);
    } else {
        pbuf_free(pbuf);
    }
}

int ipv4_send(pbuf_t* pbuf, uint32_t dst_ip, uint8_t proto) {
    if (!pbuf) return -1;

    netdev_t* dev = NULL;
    if (dst_ip == 0x0100007Fu) {
        dev = netdev_get_loopback();
    } else {
        dev = netdev_get_default();
    }
    if (!dev) return -1;

    uint32_t next_hop = dst_ip;
    if (!(dev->flags & NETDEV_FLAG_LOOPBACK)) {
        if ((dst_ip & dev->netmask) != (dev->ip & dev->netmask)) {
            next_hop = dev->gateway;
        }
    }

    uint8_t dst_mac[6];
    if (arp_resolve(dev, next_hop, dst_mac) != 0) {
        dev->tx_errors++;
        return -1;
    }

    pbuf_t* ip_pbuf = pbuf_alloc();
    if (!ip_pbuf) {
        dev->tx_dropped++;
        return -1;
    }

    struct ip_hdr* hdr = (struct ip_hdr*) ip_pbuf->payload;
    hdr->ihl = 5;
    hdr->version = 4;
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)(sizeof(struct ip_hdr) + pbuf->len));
    hdr->id = htons(s_next_id++);
    hdr->flags_offset = htons(0x4000); /* DF bit */
    hdr->ttl = 64;
    hdr->protocol = proto;
    hdr->checksum = 0;
    hdr->src_ip = dev->ip;
    hdr->dst_ip = dst_ip;

    hdr->checksum = net_checksum(hdr, sizeof(struct ip_hdr));

    memcpy(ip_pbuf->payload + sizeof(struct ip_hdr), pbuf->payload + pbuf->offset, pbuf->len);
    ip_pbuf->len = sizeof(struct ip_hdr) + pbuf->len;
    ip_pbuf->offset = 0;

    int ret = ethernet_send(dev, ip_pbuf, dst_mac, ETHERTYPE_IPV4);
    pbuf_free(ip_pbuf);
    return ret;
}

int route_get_table_text(char* buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t off = 0;

    int n = ksnprintf(buf + off, max_len - off,
        "%-16s %-16s %-8s %-16s %-8s %-8s\n",
        "Destination", "Gateway", "Flags", "Genmask", "Metric", "Iface");
    if (n > 0) off += (size_t)n;

    for (netdev_t* d = netdev_get_list(); d; d = d->next) {
        if (d->flags & NETDEV_FLAG_LOOPBACK) {
            n = ksnprintf(buf + off, max_len - off,
                "%-16s %-16s 0x0001   %-16s 0        %-8s\n",
                "127.0.0.0", "0.0.0.0", "255.0.0.0", d->name);
            if (n > 0) off += (size_t)n;
        } else {
            /* Local network route */
            char net_str[20], mask_str[20], gw_str[20];
            uint32_t net = d->ip & d->netmask;
            ksnprintf(net_str, sizeof(net_str), "%u.%u.%u.%u",
                      (net >> 0) & 0xFF, (net >> 8) & 0xFF, (net >> 16) & 0xFF, (net >> 24) & 0xFF);
            ksnprintf(mask_str, sizeof(mask_str), "%u.%u.%u.%u",
                      (d->netmask >> 0) & 0xFF, (d->netmask >> 8) & 0xFF, (d->netmask >> 16) & 0xFF, (d->netmask >> 24) & 0xFF);
            n = ksnprintf(buf + off, max_len - off,
                "%-16s %-16s 0x0001   %-16s 0        %-8s\n",
                net_str, "0.0.0.0", mask_str, d->name);
            if (n > 0) off += (size_t)n;

            /* Default gateway route */
            ksnprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u",
                      (d->gateway >> 0) & 0xFF, (d->gateway >> 8) & 0xFF, (d->gateway >> 16) & 0xFF, (d->gateway >> 24) & 0xFF);
            n = ksnprintf(buf + off, max_len - off,
                "%-16s %-16s 0x0003   %-16s 0        %-8s\n",
                "0.0.0.0", gw_str, "0.0.0.0", d->name);
            if (n > 0) off += (size_t)n;
        }
    }

    return (int)off;
}

#include "arp.h"
#include "ethernet.h"
#include "ipv4.h"
#include "timer.h"
#include "scheduler.h"
#include "console.h"
#include "serial.h"
#include <string.h>

#define ARP_TABLE_SIZE 32
#define ARP_TIMEOUT_TICKS 6000 /* 60 seconds at 100Hz */

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t timestamp;
    int valid;
} arp_entry_t;

static arp_entry_t s_arp_table[ARP_TABLE_SIZE];

void arp_init(void) {
    memset(s_arp_table, 0, sizeof(s_arp_table));
}

void arp_table_set(uint32_t ip, const uint8_t* mac) {
    if (!mac) return;

    /* Check if already present */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].valid && s_arp_table[i].ip == ip) {
            memcpy(s_arp_table[i].mac, mac, 6);
            s_arp_table[i].timestamp = timer_get_ticks();
            scheduler_wake_channel(&s_arp_table);
            return;
        }
    }

    /* Find empty or oldest slot */
    int slot = -1;
    uint32_t oldest_time = 0xFFFFFFFFu;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!s_arp_table[i].valid) {
            slot = i;
            break;
        }
        if (s_arp_table[i].timestamp < oldest_time) {
            oldest_time = s_arp_table[i].timestamp;
            slot = i;
        }
    }

    if (slot >= 0) {
        s_arp_table[slot].ip = ip;
        memcpy(s_arp_table[slot].mac, mac, 6);
        s_arp_table[slot].timestamp = timer_get_ticks();
        s_arp_table[slot].valid = 1;
        scheduler_wake_channel(&s_arp_table);
    }
}

void arp_rx(netdev_t* dev, pbuf_t* pbuf) {
    if (!dev || !pbuf) return;

    if (pbuf->len < sizeof(struct arp_hdr)) {
        dev->rx_errors++;
        pbuf_free(pbuf);
        return;
    }

    struct arp_hdr* arp = (struct arp_hdr*)(pbuf->payload + pbuf->offset);
    uint16_t hw_type = ntohs(arp->hw_type);
    uint16_t proto_type = ntohs(arp->proto_type);
    uint16_t op = ntohs(arp->op);

    if (hw_type != 1 || proto_type != 0x0800 || arp->hw_len != 6 || arp->proto_len != 4) {
        pbuf_free(pbuf);
        return;
    }

    /* Update ARP cache with sender info */
    arp_table_set(arp->src_ip, arp->src_mac);

    if (op == ARP_OP_REQUEST && arp->dst_ip == dev->ip) {
        /* Build ARP reply */
        pbuf_t* reply = pbuf_alloc();
        if (reply) {
            struct arp_hdr* rep_hdr = (struct arp_hdr*) reply->payload;
            rep_hdr->hw_type = htons(1);
            rep_hdr->proto_type = htons(0x0800);
            rep_hdr->hw_len = 6;
            rep_hdr->proto_len = 4;
            rep_hdr->op = htons(ARP_OP_REPLY);

            memcpy(rep_hdr->src_mac, dev->mac, 6);
            rep_hdr->src_ip = dev->ip;
            memcpy(rep_hdr->dst_mac, arp->src_mac, 6);
            rep_hdr->dst_ip = arp->src_ip;

            reply->len = sizeof(struct arp_hdr);
            reply->offset = 0;

            ethernet_send(dev, reply, arp->src_mac, ETHERTYPE_ARP);
            pbuf_free(reply);
        }
    }

    pbuf_free(pbuf);
}

int arp_resolve(netdev_t* dev, uint32_t ip, uint8_t* out_mac) {
    if (!dev || !out_mac) return -1;

    /* Loopback or broadcast */
    if (dev->flags & NETDEV_FLAG_LOOPBACK) {
        memset(out_mac, 0, 6);
        return 0;
    }
    if (ip == 0xFFFFFFFFu) {
        memset(out_mac, 0xFF, 6);
        return 0;
    }

    /* Check cache */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (s_arp_table[i].valid && s_arp_table[i].ip == ip) {
            memcpy(out_mac, s_arp_table[i].mac, 6);
            return 0;
        }
    }

    /* Send ARP Request up to 3 times */
    for (int retry = 0; retry < 3; retry++) {
        pbuf_t* req = pbuf_alloc();
        if (!req) return -1;

        struct arp_hdr* hdr = (struct arp_hdr*) req->payload;
        hdr->hw_type = htons(1);
        hdr->proto_type = htons(0x0800);
        hdr->hw_len = 6;
        hdr->proto_len = 4;
        hdr->op = htons(ARP_OP_REQUEST);

        memcpy(hdr->src_mac, dev->mac, 6);
        hdr->src_ip = dev->ip;
        memset(hdr->dst_mac, 0, 6);
        hdr->dst_ip = ip;

        req->len = sizeof(struct arp_hdr);
        req->offset = 0;

        ethernet_send(dev, req, NULL, ETHERTYPE_ARP);
        pbuf_free(req);

        /* Wait for response (up to 50 ticks = 500ms per attempt) */
        uint32_t start_tick = timer_get_ticks();
        while (timer_get_ticks() - start_tick < 50) {
            for (int i = 0; i < ARP_TABLE_SIZE; i++) {
                if (s_arp_table[i].valid && s_arp_table[i].ip == ip) {
                    memcpy(out_mac, s_arp_table[i].mac, 6);
                    return 0;
                }
            }
            process_t* me = scheduler_current();
            if (me) {
                timer_sleep_enqueue(me, timer_get_ticks() + 2);
                scheduler_wait_channel(&s_arp_table);
                timer_sleep_dequeue(me);
            } else {
                scheduler_yield();
            }
        }
    }

    return -1;
}

int arp_get_table_text(char* buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t off = 0;

    int n = ksnprintf(buf + off, max_len - off,
        "%-16s %-8s %-8s %-18s %-8s %-8s\n",
        "IP address", "HW type", "Flags", "HW address", "Mask", "Device");
    if (n > 0) off += (size_t)n;

    for (int i = 0; i < ARP_TABLE_SIZE && off + 80 < max_len; i++) {
        if (!s_arp_table[i].valid) continue;
        uint32_t ip = s_arp_table[i].ip;
        const uint8_t* m = s_arp_table[i].mac;

        char ip_str[20];
        ksnprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                  (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

        char mac_str[24];
        ksnprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                  m[0], m[1], m[2], m[3], m[4], m[5]);

        n = ksnprintf(buf + off, max_len - off,
            "%-16s 0x1      0x2      %-18s *        eth0\n",
            ip_str, mac_str);
        if (n > 0) off += (size_t)n;
    }

    return (int)off;
}

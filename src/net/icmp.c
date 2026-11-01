#include "icmp.h"
#include "ipv4.h"
#include "timer.h"
#include <string.h>

void icmp_init(void) {
}

void icmp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip) {
    if (!dev || !pbuf || !ip) return;

    if (pbuf->len < sizeof(struct icmp_hdr)) {
        pbuf_free(pbuf);
        return;
    }

    struct icmp_hdr* icmp = (struct icmp_hdr*)(pbuf->payload + pbuf->offset);

    /* Verify checksum */
    uint16_t orig_csum = icmp->checksum;
    icmp->checksum = 0;
    uint16_t calc_csum = net_checksum(icmp, pbuf->len);
    icmp->checksum = orig_csum;
    if (orig_csum != calc_csum) {
        pbuf_free(pbuf);
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        /* Generate echo reply */
        pbuf_t* reply = pbuf_alloc();
        if (reply) {
            memcpy(reply->payload, pbuf->payload + pbuf->offset, pbuf->len);
            struct icmp_hdr* rep_hdr = (struct icmp_hdr*) reply->payload;
            rep_hdr->type = ICMP_TYPE_ECHO_REPLY;
            rep_hdr->code = 0;
            rep_hdr->checksum = 0;
            rep_hdr->checksum = net_checksum(rep_hdr, pbuf->len);

            reply->len = pbuf->len;
            reply->offset = 0;

            ipv4_send(reply, ip->src_ip, IPPROTO_ICMP);
            pbuf_free(reply);
        }
    }

    pbuf_free(pbuf);
}

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq, const void* data, size_t len) {
    size_t total_len = sizeof(struct icmp_hdr) + len;
    if (total_len > PBUF_PAYLOAD_SIZE) return -1;

    pbuf_t* p = pbuf_alloc();
    if (!p) return -1;

    struct icmp_hdr* hdr = (struct icmp_hdr*) p->payload;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->seq = htons(seq);

    if (data && len > 0) {
        memcpy(p->payload + sizeof(struct icmp_hdr), data, len);
    }

    hdr->checksum = net_checksum(hdr, total_len);
    p->len = total_len;
    p->offset = 0;

    int ret = ipv4_send(p, dst_ip, IPPROTO_ICMP);
    pbuf_free(p);
    return ret;
}


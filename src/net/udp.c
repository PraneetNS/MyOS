#include "udp.h"
#include "ipv4.h"
#include "kheap.h"
#include "scheduler.h"
#include "timer.h"
#include "errno.h"
#include "console.h"
#include "serial.h"
#include <string.h>

#define UDP_MAX_SOCKETS 64

typedef struct udp_pkt {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    struct udp_pkt* next;
    uint8_t data[];
} udp_pkt_t;

typedef struct {
    udp_pkt_t* queue_head;
    udp_pkt_t* queue_tail;
    int queue_count;
} udp_sock_data_t;

static socket_t* s_udp_sockets[UDP_MAX_SOCKETS];
static uint16_t s_next_ephemeral = 49152;

void udp_init(void) {
    memset(s_udp_sockets, 0, sizeof(s_udp_sockets));
}

static uint16_t alloc_ephemeral_port(void) {
    for (int i = 0; i < 16384; i++) {
        uint16_t port = s_next_ephemeral++;
        if (s_next_ephemeral >= 65535) s_next_ephemeral = 49152;

        int used = 0;
        for (int j = 0; j < UDP_MAX_SOCKETS; j++) {
            if (s_udp_sockets[j] && s_udp_sockets[j]->local_port == port) {
                used = 1;
                break;
            }
        }
        if (!used) return port;
    }
    return 0;
}

static int udp_bind(socket_t* sock, const struct sockaddr* addr, uint32_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in* sin = (const struct sockaddr_in*) addr;
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;

    uint16_t port = ntohs(sin->sin_port);
    kmutex_lock(&sock->lock);

    if (sock->local_port != 0) {
        kmutex_unlock(&sock->lock);
        return -EINVAL; /* Already bound */
    }

    /* Check port collision */
    if (port != 0 && !sock->reuseaddr) {
        for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
            if (s_udp_sockets[i] && s_udp_sockets[i] != sock && s_udp_sockets[i]->local_port == port) {
                kmutex_unlock(&sock->lock);
                return -EADDRINUSE;
            }
        }
    }

    if (port == 0) {
        port = alloc_ephemeral_port();
        if (port == 0) {
            kmutex_unlock(&sock->lock);
            return -EADDRINUSE;
        }
    }

    /* Add to table */
    int slot = -1;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!s_udp_sockets[i] || s_udp_sockets[i] == sock) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kmutex_unlock(&sock->lock);
        return -ENOMEM;
    }

    sock->local_ip = sin->sin_addr.s_addr;
    sock->local_port = port;
    s_udp_sockets[slot] = sock;

    if (!sock->inet_data) {
        udp_sock_data_t* d = (udp_sock_data_t*) kmalloc(sizeof(udp_sock_data_t));
        if (d) {
            memset(d, 0, sizeof(udp_sock_data_t));
            sock->inet_data = d;
        }
    }

    kmutex_unlock(&sock->lock);
    return 0;
}

static int udp_sendto(socket_t* sock, const void* buf, uint32_t len, int flags, const struct sockaddr* dest_addr, uint32_t addrlen) {
    (void)flags;
    if (!sock || !buf) return -EINVAL;

    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;

    if (dest_addr) {
        if (addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
        const struct sockaddr_in* sin = (const struct sockaddr_in*) dest_addr;
        if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;
        dst_ip = sin->sin_addr.s_addr;
        dst_port = ntohs(sin->sin_port);
    } else {
        if (sock->remote_ip == 0 || sock->remote_port == 0) return -ENOTCONN;
        dst_ip = sock->remote_ip;
        dst_port = sock->remote_port;
    }

    if (sock->local_port == 0) {
        /* Auto-bind */
        struct sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port = 0;
        sin.sin_addr.s_addr = 0;
        int err = udp_bind(sock, (struct sockaddr*)&sin, sizeof(sin));
        if (err < 0) return err;
    }

    kmutex_lock(&sock->lock);
    uint16_t src_port = sock->local_port;
    kmutex_unlock(&sock->lock);

    if (len + sizeof(struct udp_hdr) > PBUF_PAYLOAD_SIZE) return -EMSGSIZE;

    pbuf_t* p = pbuf_alloc();
    if (!p) return -ENOBUFS;

    struct udp_hdr* hdr = (struct udp_hdr*) p->payload;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->len = htons((uint16_t)(sizeof(struct udp_hdr) + len));
    hdr->checksum = 0;

    memcpy(p->payload + sizeof(struct udp_hdr), buf, len);
    p->len = sizeof(struct udp_hdr) + len;
    p->offset = 0;

    int ret = ipv4_send(p, dst_ip, IPPROTO_UDP);
    pbuf_free(p);

    return (ret == 0) ? (int)len : ret;
}

static int udp_send(socket_t* sock, const void* buf, uint32_t len, int flags) {
    return udp_sendto(sock, buf, len, flags, NULL, 0);
}

static int udp_recvfrom(socket_t* sock, void* buf, uint32_t len, int flags, struct sockaddr* src_addr, uint32_t* addrlen) {
    if (!sock || !buf) return -EINVAL;

    kmutex_lock(&sock->lock);
    if (!sock->inet_data) {
        udp_sock_data_t* d = (udp_sock_data_t*) kmalloc(sizeof(udp_sock_data_t));
        if (!d) {
            kmutex_unlock(&sock->lock);
            return -ENOMEM;
        }
        memset(d, 0, sizeof(udp_sock_data_t));
        sock->inet_data = d;
    }
    udp_sock_data_t* d = (udp_sock_data_t*) sock->inet_data;

    int nonblock = (flags & MSG_DONTWAIT) || (sock->flags & SOCK_NONBLOCK);
    uint32_t start_time = timer_get_ticks();
    uint32_t timeout_ticks = sock->rcvtimeo_ms ? (sock->rcvtimeo_ms / 10 + 1) : 0;

    while (!d->queue_head) {
        if (sock->shutdown_rd) {
            kmutex_unlock(&sock->lock);
            return 0;
        }
        if (nonblock) {
            kmutex_unlock(&sock->lock);
            return -EAGAIN;
        }
        if (timeout_ticks > 0 && (timer_get_ticks() - start_time >= timeout_ticks)) {
            kmutex_unlock(&sock->lock);
            return -ETIMEDOUT;
        }

        kmutex_unlock(&sock->lock);
        scheduler_wait_channel(&sock->rx_ring.read_wait);
        kmutex_lock(&sock->lock);
    }

    udp_pkt_t* pkt = d->queue_head;
    d->queue_head = pkt->next;
    if (!d->queue_head) d->queue_tail = NULL;
    d->queue_count--;

    uint32_t to_copy = (pkt->len < len) ? pkt->len : len;
    memcpy(buf, pkt->data, to_copy);

    if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in* sin = (struct sockaddr_in*) src_addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(pkt->src_port);
        sin->sin_addr.s_addr = pkt->src_ip;
        memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
        *addrlen = sizeof(struct sockaddr_in);
    }

    kfree(pkt);
    kmutex_unlock(&sock->lock);
    return (int)to_copy;
}

static int udp_recv(socket_t* sock, void* buf, uint32_t len, int flags) {
    return udp_recvfrom(sock, buf, len, flags, NULL, NULL);
}

static int udp_connect(socket_t* sock, const struct sockaddr* addr, uint32_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in* sin = (const struct sockaddr_in*) addr;
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;

    kmutex_lock(&sock->lock);
    sock->remote_ip = sin->sin_addr.s_addr;
    sock->remote_port = ntohs(sin->sin_port);
    sock->state = SS_CONNECTED;
    kmutex_unlock(&sock->lock);
    return 0;
}

static short udp_poll(socket_t* sock, short events, void** chan_out) {
    if (!sock) return 0;
    short revents = 0;

    kmutex_lock(&sock->lock);
    udp_sock_data_t* d = (udp_sock_data_t*) sock->inet_data;
    if ((events & 1) /* POLLIN */) {
        if ((d && d->queue_head) || sock->shutdown_rd) {
            revents |= 1;
        } else if (chan_out) {
            *chan_out = &sock->rx_ring.read_wait;
        }
    }
    if ((events & 4) /* POLLOUT */) {
        revents |= 4;
    }
    kmutex_unlock(&sock->lock);
    return revents;
}

static void udp_close(socket_t* sock) {
    if (!sock) return;

    kmutex_lock(&sock->lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (s_udp_sockets[i] == sock) {
            s_udp_sockets[i] = NULL;
            break;
        }
    }

    if (sock->inet_data) {
        udp_sock_data_t* d = (udp_sock_data_t*) sock->inet_data;
        udp_pkt_t* curr = d->queue_head;
        while (curr) {
            udp_pkt_t* next = curr->next;
            kfree(curr);
            curr = next;
        }
        kfree(d);
        sock->inet_data = NULL;
    }
    kmutex_unlock(&sock->lock);
}

static int udp_getsockname(socket_t* sock, struct sockaddr* addr, uint32_t* addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(sock->local_port);
    sin->sin_addr.s_addr = sock->local_ip;
    memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int udp_getpeername(socket_t* sock, struct sockaddr* addr, uint32_t* addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    if (sock->remote_ip == 0 || sock->remote_port == 0) return -ENOTCONN;
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(sock->remote_port);
    sin->sin_addr.s_addr = sock->remote_ip;
    memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int udp_setsockopt(socket_t* sock, int level, int optname, const void* optval, uint32_t optlen) {
    if (!sock || !optval) return -EINVAL;
    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR && optlen >= sizeof(int)) {
            sock->reuseaddr = *(const int*)optval;
            return 0;
        }
        if (optname == SO_RCVTIMEO && optlen >= sizeof(struct timeval)) {
            const struct timeval* tv = (const struct timeval*) optval;
            sock->rcvtimeo_ms = (uint32_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            return 0;
        }
    }
    return -ENOPROTOOPT;
}

static int udp_getsockopt(socket_t* sock, int level, int optname, void* optval, uint32_t* optlen) {
    if (!sock || !optval || !optlen) return -EINVAL;
    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR && *optlen >= sizeof(int)) {
            *(int*)optval = sock->reuseaddr;
            *optlen = sizeof(int);
            return 0;
        }
    }
    return -ENOPROTOOPT;
}

const struct socket_ops udp_socket_ops = {
    .bind = udp_bind,
    .listen = NULL,
    .accept = NULL,
    .connect = udp_connect,
    .send = udp_send,
    .recv = udp_recv,
    .sendto = udp_sendto,
    .recvfrom = udp_recvfrom,
    .shutdown = NULL,
    .poll = udp_poll,
    .close = udp_close,
    .getsockname = udp_getsockname,
    .getpeername = udp_getpeername,
    .setsockopt = udp_setsockopt,
    .getsockopt = udp_getsockopt
};

void udp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip) {
    (void)dev;
    if (!pbuf || !ip) return;

    if (pbuf->len < sizeof(struct udp_hdr)) {
        pbuf_free(pbuf);
        return;
    }

    struct udp_hdr* udp = (struct udp_hdr*)(pbuf->payload + pbuf->offset);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t ulen = ntohs(udp->len);

    if (ulen < sizeof(struct udp_hdr) || ulen > pbuf->len) {
        pbuf_free(pbuf);
        return;
    }

    uint16_t data_len = ulen - sizeof(struct udp_hdr);
    const uint8_t* data = pbuf->payload + pbuf->offset + sizeof(struct udp_hdr);

    /* Find matching socket */
    socket_t* target = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        socket_t* s = s_udp_sockets[i];
        if (s && s->local_port == dst_port) {
            target = s;
            break;
        }
    }

    if (target) {
        kmutex_lock(&target->lock);
        if (!target->inet_data) {
            udp_sock_data_t* d = (udp_sock_data_t*) kmalloc(sizeof(udp_sock_data_t));
            if (d) {
                memset(d, 0, sizeof(udp_sock_data_t));
                target->inet_data = d;
            }
        }
        udp_sock_data_t* d = (udp_sock_data_t*) target->inet_data;
        if (d && d->queue_count < 64) {
            udp_pkt_t* pkt = (udp_pkt_t*) kmalloc(sizeof(udp_pkt_t) + data_len);
            if (pkt) {
                pkt->src_ip = ip->src_ip;
                pkt->src_port = src_port;
                pkt->len = data_len;
                pkt->next = NULL;
                memcpy(pkt->data, data, data_len);

                if (d->queue_tail) {
                    d->queue_tail->next = pkt;
                } else {
                    d->queue_head = pkt;
                }
                d->queue_tail = pkt;
                d->queue_count++;

                scheduler_wake_channel(&target->rx_ring.read_wait);
            }
        }
        kmutex_unlock(&target->lock);
    }

    pbuf_free(pbuf);
}

int udp_get_table_text(char* buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t off = 0;

    int n = ksnprintf(buf + off, max_len - off,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    if (n > 0) off += (size_t)n;

    int sl = 0;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        socket_t* s = s_udp_sockets[i];
        if (!s) continue;

        char loc[32], rem[32];
        ksnprintf(loc, sizeof(loc), "%08X:%04X", s->local_ip, s->local_port);
        ksnprintf(rem, sizeof(rem), "%08X:%04X", s->remote_ip, s->remote_port);

        n = ksnprintf(buf + off, max_len - off,
            "%4d: %-13s %-13s 07 00000000:00000000 00:00000000 00000000     0        0 %u 1 0\n",
            sl++, loc, rem, s->inode);
        if (n > 0) off += (size_t)n;
    }

    return (int)off;
}

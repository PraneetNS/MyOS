#include "tcp.h"
#include "ipv4.h"
#include "kheap.h"
#include "scheduler.h"
#include "timer.h"
#include "errno.h"
#include "console.h"
#include "serial.h"
#include <string.h>

#define TCP_MAX_SOCKETS 64
#define TCP_RX_BUF_SIZE (32 * 1024)
#define TCP_MSS         1460

enum tcp_state {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
};

typedef struct tcp_cb {
    int state;
    uint32_t snd_una;     /* Oldest unacknowledged sequence number */
    uint32_t snd_nxt;     /* Next sequence number to send */
    uint32_t snd_wnd;     /* Peer's receive window */
    uint32_t rcv_nxt;     /* Next sequence number we expect to receive */
    uint32_t rcv_wnd;     /* Our receive window */
    uint32_t iss;         /* Initial send sequence number */
    uint32_t irs;         /* Initial receive sequence number */

    int connect_wait;
    int connect_status;

    socket_t* parent;     /* For accepted sockets */

    uint8_t* rx_buf;
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
} tcp_cb_t;

static socket_t* s_tcp_sockets[TCP_MAX_SOCKETS];
static uint16_t s_next_ephemeral = 49152;

void tcp_init(void) {
    memset(s_tcp_sockets, 0, sizeof(s_tcp_sockets));
}

static uint16_t alloc_ephemeral_port(void) {
    for (int i = 0; i < 16384; i++) {
        uint16_t port = s_next_ephemeral++;
        if (s_next_ephemeral >= 65535) s_next_ephemeral = 49152;

        int used = 0;
        for (int j = 0; j < TCP_MAX_SOCKETS; j++) {
            if (s_tcp_sockets[j] && s_tcp_sockets[j]->local_port == port) {
                used = 1;
                break;
            }
        }
        if (!used) return port;
    }
    return 0;
}

static tcp_cb_t* get_or_create_tcb(socket_t* sock) {
    if (!sock->inet_data) {
        tcp_cb_t* tcb = (tcp_cb_t*) kmalloc(sizeof(tcp_cb_t));
        if (!tcb) return NULL;
        memset(tcb, 0, sizeof(tcp_cb_t));
        tcb->state = TCP_STATE_CLOSED;
        tcb->rcv_wnd = TCP_RX_BUF_SIZE;
        tcb->snd_wnd = 8192;
        tcb->rx_buf = (uint8_t*) kmalloc(TCP_RX_BUF_SIZE);
        if (!tcb->rx_buf) {
            kfree(tcb);
            return NULL;
        }
        sock->inet_data = tcb;
    }
    return (tcp_cb_t*) sock->inet_data;
}

static int tcp_send_segment(socket_t* sock, uint8_t flags, uint32_t seq, uint32_t ack, const void* data, uint32_t len) {
    if (!sock) return -1;

    pbuf_t* p = pbuf_alloc();
    if (!p) return -ENOBUFS;

    struct tcp_hdr* hdr = (struct tcp_hdr*) p->payload;
    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack);
    hdr->data_offset = (5 << 4); /* 5 32-bit words = 20 bytes */
    hdr->flags = flags;
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;
    uint32_t free_rx = tcb ? (TCP_RX_BUF_SIZE - tcb->rx_count) : 8192;
    hdr->window = htons((uint16_t)(free_rx > 65535 ? 65535 : free_rx));
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && len > 0) {
        memcpy(p->payload + sizeof(struct tcp_hdr), data, len);
    }

    uint32_t total_tcp_len = sizeof(struct tcp_hdr) + len;
    hdr->checksum = net_checksum_pseudo(hdr, total_tcp_len, sock->local_ip, sock->remote_ip, IPPROTO_TCP);

    p->len = total_tcp_len;
    p->offset = 0;

    int ret = ipv4_send(p, sock->remote_ip, IPPROTO_TCP);
    pbuf_free(p);
    return ret;
}

static int tcp_bind(socket_t* sock, const struct sockaddr* addr, uint32_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in* sin = (const struct sockaddr_in*) addr;
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;

    uint16_t port = ntohs(sin->sin_port);
    kmutex_lock(&sock->lock);

    if (sock->local_port != 0) {
        kmutex_unlock(&sock->lock);
        return -EINVAL;
    }

    if (port != 0 && !sock->reuseaddr) {
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (s_tcp_sockets[i] && s_tcp_sockets[i] != sock && s_tcp_sockets[i]->local_port == port) {
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

    int slot = -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!s_tcp_sockets[i] || s_tcp_sockets[i] == sock) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kmutex_unlock(&sock->lock);
        return -ENOMEM;
    }

    sock->local_ip = sin->sin_addr.s_addr;
    if (sock->local_ip == 0) {
        netdev_t* def = netdev_get_default();
        if (def) sock->local_ip = def->ip;
    }
    sock->local_port = port;
    s_tcp_sockets[slot] = sock;

    get_or_create_tcb(sock);

    kmutex_unlock(&sock->lock);
    return 0;
}

static int tcp_listen(socket_t* sock, int backlog) {
    if (!sock) return -EINVAL;
    kmutex_lock(&sock->lock);

    if (sock->local_port == 0) {
        kmutex_unlock(&sock->lock);
        return -EINVAL;
    }

    tcp_cb_t* tcb = get_or_create_tcb(sock);
    if (!tcb) {
        kmutex_unlock(&sock->lock);
        return -ENOMEM;
    }

    tcb->state = TCP_STATE_LISTEN;
    sock->state = SS_LISTENING;
    sock->accept_head = 0;
    sock->accept_tail = 0;
    sock->accept_count = 0;
    sock->accept_backlog = (backlog > 0 && backlog <= UNIX_BACKLOG_MAX) ? backlog : 5;

    kmutex_unlock(&sock->lock);
    return 0;
}

static int tcp_accept(socket_t* sock, struct sockaddr* addr, uint32_t* addrlen, int flags, socket_t** out) {
    if (!sock || !out) return -EINVAL;

    kmutex_lock(&sock->lock);
    if (sock->state != SS_LISTENING) {
        kmutex_unlock(&sock->lock);
        return -EINVAL;
    }

    int nonblock = (flags & SOCK_NONBLOCK) || (sock->flags & SOCK_NONBLOCK);

    while (sock->accept_count == 0) {
        if (sock->shutdown_rd) {
            kmutex_unlock(&sock->lock);
            return -EINVAL;
        }
        if (nonblock) {
            kmutex_unlock(&sock->lock);
            return -EAGAIN;
        }
        kmutex_unlock(&sock->lock);
        scheduler_wait_channel(&sock->accept_wait);
        kmutex_lock(&sock->lock);
    }

    socket_t* client = sock->accept_queue[sock->accept_head];
    sock->accept_head = (sock->accept_head + 1) % UNIX_BACKLOG_MAX;
    sock->accept_count--;

    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in* sin = (struct sockaddr_in*) addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(client->remote_port);
        sin->sin_addr.s_addr = client->remote_ip;
        memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
        *addrlen = sizeof(struct sockaddr_in);
    }

    *out = client;
    kmutex_unlock(&sock->lock);
    return 0;
}

static int tcp_connect(socket_t* sock, const struct sockaddr* addr, uint32_t addrlen) {
    if (!sock || !addr || addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in* sin = (const struct sockaddr_in*) addr;
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;

    if (sock->local_port == 0) {
        struct sockaddr_in local_sin;
        local_sin.sin_family = AF_INET;
        local_sin.sin_port = 0;
        local_sin.sin_addr.s_addr = 0;
        int err = tcp_bind(sock, (struct sockaddr*)&local_sin, sizeof(local_sin));
        if (err < 0) return err;
    }

    kmutex_lock(&sock->lock);

    sock->remote_ip = sin->sin_addr.s_addr;
    sock->remote_port = ntohs(sin->sin_port);

    if (sock->local_ip == 0) {
        if (sock->remote_ip == 0x0100007Fu) {
            sock->local_ip = 0x0100007Fu;
        } else {
            netdev_t* def = netdev_get_default();
            if (def) sock->local_ip = def->ip;
        }
    }

    tcp_cb_t* tcb = get_or_create_tcb(sock);
    if (!tcb) {
        kmutex_unlock(&sock->lock);
        return -ENOMEM;
    }

    tcb->iss = timer_get_ticks() * 1000 + (uint32_t)(uintptr_t)sock;
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->state = TCP_STATE_SYN_SENT;
    tcb->connect_status = 0;

    /* Send SYN */
    int sret = tcp_send_segment(sock, TCP_SYN, tcb->snd_nxt, 0, NULL, 0);
    tcb->snd_nxt++;

    /* Wait for 3-way handshake completion */
    uint32_t start_time = timer_get_ticks();
    uint32_t timeout_ticks = 300; /* 3 seconds */
    uint32_t last_syn_tick = start_time;

    while (tcb->state == TCP_STATE_SYN_SENT) {
        if (tcb->state == TCP_STATE_ESTABLISHED) break;

        if (timer_get_ticks() - start_time > timeout_ticks) {
            tcb->state = TCP_STATE_CLOSED;
            kmutex_unlock(&sock->lock);
            return -ETIMEDOUT;
        }

        /* Retransmit SYN if unacknowledged (every 500ms or immediately if first attempt failed) */
        if (sret < 0 || (timer_get_ticks() - last_syn_tick >= 50)) {
            sret = tcp_send_segment(sock, TCP_SYN, tcb->iss, 0, NULL, 0);
            last_syn_tick = timer_get_ticks();
        }

        process_t* me = scheduler_current();
        if (me) timer_sleep_enqueue(me, timer_get_ticks() + 25);

        kmutex_unlock(&sock->lock);
        scheduler_wait_channel(&tcb->connect_wait);
        kmutex_lock(&sock->lock);

        if (me) timer_sleep_dequeue(me);
    }

    if (tcb->state != TCP_STATE_ESTABLISHED) {
        kmutex_unlock(&sock->lock);
        return -ECONNREFUSED;
    }

    sock->state = SS_CONNECTED;
    kmutex_unlock(&sock->lock);
    return 0;
}

static int tcp_send(socket_t* sock, const void* buf, uint32_t len, int flags) {
    (void)flags;
    if (!sock || !buf) return -EINVAL;

    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;
    if (!tcb || tcb->state != TCP_STATE_ESTABLISHED) {
        kmutex_unlock(&sock->lock);
        return -ENOTCONN;
    }

    const uint8_t* ptr = (const uint8_t*) buf;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint32_t chunk = (remaining > TCP_MSS) ? TCP_MSS : remaining;
        uint32_t seq = tcb->snd_nxt;

        int ret = tcp_send_segment(sock, TCP_ACK | TCP_PSH, seq, tcb->rcv_nxt, ptr, chunk);
        if (ret < 0) {
            kmutex_unlock(&sock->lock);
            return ret;
        }
        tcb->snd_nxt += chunk;

        /* Wait for ACK or retransmit (up to 200ms per chunk) */
        uint32_t send_tick = timer_get_ticks();
        while (tcb->snd_una < tcb->snd_nxt) {
            if (timer_get_ticks() - send_tick > 20) { /* 200ms */
                tcp_send_segment(sock, TCP_ACK | TCP_PSH, seq, tcb->rcv_nxt, ptr, chunk);
                send_tick = timer_get_ticks();
            }
            if (tcb->state != TCP_STATE_ESTABLISHED) break;

            process_t* me = scheduler_current();
            if (me) timer_sleep_enqueue(me, timer_get_ticks() + 10);

            kmutex_unlock(&sock->lock);
            scheduler_wait_channel(&sock->rx_ring.write_wait);
            kmutex_lock(&sock->lock);

            if (me) timer_sleep_dequeue(me);
        }

        ptr += chunk;
        remaining -= chunk;
    }

    kmutex_unlock(&sock->lock);
    return (int)len;
}

static int tcp_recv(socket_t* sock, void* buf, uint32_t len, int flags) {
    if (!sock || !buf) return -EINVAL;

    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;
    if (!tcb) {
        kmutex_unlock(&sock->lock);
        return -ENOTCONN;
    }

    int nonblock = (flags & MSG_DONTWAIT) || (sock->flags & SOCK_NONBLOCK);
    uint32_t start_time = timer_get_ticks();
    uint32_t timeout_ticks = sock->rcvtimeo_ms ? (sock->rcvtimeo_ms / 10 + 1) : 0;

    while (tcb->rx_count == 0) {
        if (tcb->state == TCP_STATE_CLOSE_WAIT || tcb->state == TCP_STATE_CLOSED || sock->shutdown_rd) {
            kmutex_unlock(&sock->lock);
            return 0; /* EOF */
        }
        if (nonblock) {
            kmutex_unlock(&sock->lock);
            return -EAGAIN;
        }
        if (timeout_ticks > 0 && (timer_get_ticks() - start_time >= timeout_ticks)) {
            kmutex_unlock(&sock->lock);
            return -ETIMEDOUT;
        }

        process_t* me = scheduler_current();
        uint32_t wait_ticks = (timeout_ticks > 0) ? (timeout_ticks - (timer_get_ticks() - start_time)) : 50;
        if (wait_ticks > 50) wait_ticks = 50;
        if (wait_ticks == 0) wait_ticks = 1;
        if (me) timer_sleep_enqueue(me, timer_get_ticks() + wait_ticks);

        kmutex_unlock(&sock->lock);
        scheduler_wait_channel(&sock->rx_ring.read_wait);
        kmutex_lock(&sock->lock);

        if (me) timer_sleep_dequeue(me);
    }

    uint32_t to_copy = (tcb->rx_count < len) ? tcb->rx_count : len;
    for (uint32_t i = 0; i < to_copy; i++) {
        ((uint8_t*)buf)[i] = tcb->rx_buf[tcb->rx_head];
        tcb->rx_head = (tcb->rx_head + 1) % TCP_RX_BUF_SIZE;
    }
    tcb->rx_count -= to_copy;

    /* If window was small and now opened, send an update ACK */
    if (tcb->state == TCP_STATE_ESTABLISHED && (TCP_RX_BUF_SIZE - tcb->rx_count >= TCP_MSS)) {
        tcp_send_segment(sock, TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
    }

    kmutex_unlock(&sock->lock);
    return (int)to_copy;
}

static int tcp_shutdown(socket_t* sock, int how) {
    if (!sock) return -EINVAL;
    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;
    if (tcb && tcb->state == TCP_STATE_ESTABLISHED) {
        if (how == SHUT_WR || how == SHUT_RDWR) {
            tcp_send_segment(sock, TCP_FIN | TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            tcb->snd_nxt++;
            tcb->state = TCP_STATE_FIN_WAIT_1;
        }
        if (how == SHUT_RD || how == SHUT_RDWR) {
            sock->shutdown_rd = 1;
            scheduler_wake_channel(&sock->rx_ring.read_wait);
        }
    }
    kmutex_unlock(&sock->lock);
    return 0;
}

static short tcp_poll(socket_t* sock, short events, void** chan_out) {
    if (!sock) return 0;
    short revents = 0;

    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;

    if (sock->state == SS_LISTENING) {
        if (sock->accept_count > 0) {
            revents |= 1; /* POLLIN */
        } else if (chan_out) {
            *chan_out = &sock->accept_wait;
        }
        kmutex_unlock(&sock->lock);
        return revents;
    }

    if ((events & 1) /* POLLIN */) {
        if ((tcb && tcb->rx_count > 0) || sock->shutdown_rd || (tcb && tcb->state == TCP_STATE_CLOSE_WAIT)) {
            revents |= 1;
        } else if (chan_out) {
            *chan_out = &sock->rx_ring.read_wait;
        }
    }
    if ((events & 4) /* POLLOUT */) {
        if (tcb && tcb->state == TCP_STATE_ESTABLISHED) {
            revents |= 4;
        }
    }
    kmutex_unlock(&sock->lock);
    return revents;
}

static void tcp_close(socket_t* sock) {
    if (!sock) return;

    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;
    if (tcb) {
        if (tcb->state == TCP_STATE_ESTABLISHED) {
            tcp_send_segment(sock, TCP_FIN | TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            tcb->snd_nxt++;
            tcb->state = TCP_STATE_FIN_WAIT_1;
        } else if (tcb->state == TCP_STATE_CLOSE_WAIT) {
            tcp_send_segment(sock, TCP_FIN | TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            tcb->snd_nxt++;
            tcb->state = TCP_STATE_LAST_ACK;
        }
    }

    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (s_tcp_sockets[i] == sock) {
            s_tcp_sockets[i] = NULL;
            break;
        }
    }

    if (tcb) {
        if (tcb->rx_buf) kfree(tcb->rx_buf);
        kfree(tcb);
        sock->inet_data = NULL;
    }
    kmutex_unlock(&sock->lock);
}

static int tcp_getsockname(socket_t* sock, struct sockaddr* addr, uint32_t* addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(sock->local_port);
    sin->sin_addr.s_addr = sock->local_ip;
    memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int tcp_getpeername(socket_t* sock, struct sockaddr* addr, uint32_t* addrlen) {
    if (!sock || !addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    if (sock->state != SS_CONNECTED && sock->remote_ip == 0) return -ENOTCONN;
    struct sockaddr_in* sin = (struct sockaddr_in*) addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(sock->remote_port);
    sin->sin_addr.s_addr = sock->remote_ip;
    memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
    *addrlen = sizeof(struct sockaddr_in);
    return 0;
}

static int tcp_setsockopt(socket_t* sock, int level, int optname, const void* optval, uint32_t optlen) {
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

static int tcp_getsockopt(socket_t* sock, int level, int optname, void* optval, uint32_t* optlen) {
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

const struct socket_ops tcp_socket_ops = {
    .bind = tcp_bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .connect = tcp_connect,
    .send = tcp_send,
    .recv = tcp_recv,
    .sendto = NULL,
    .recvfrom = NULL,
    .shutdown = tcp_shutdown,
    .poll = tcp_poll,
    .close = tcp_close,
    .getsockname = tcp_getsockname,
    .getpeername = tcp_getpeername,
    .setsockopt = tcp_setsockopt,
    .getsockopt = tcp_getsockopt
};

void tcp_rx(netdev_t* dev, pbuf_t* pbuf, const struct ip_hdr* ip) {
    (void)dev;
    if (!pbuf || !ip) return;

    if (pbuf->len < sizeof(struct tcp_hdr)) {
        pbuf_free(pbuf);
        return;
    }

    struct tcp_hdr* tcp = (struct tcp_hdr*)(pbuf->payload + pbuf->offset);
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);
    uint8_t flags = tcp->flags;
    uint32_t hdr_len = (tcp->data_offset >> 4) * 4;

    if (hdr_len < sizeof(struct tcp_hdr) || hdr_len > pbuf->len) {
        pbuf_free(pbuf);
        return;
    }

    uint32_t payload_len = pbuf->len - hdr_len;
    const uint8_t* payload = pbuf->payload + pbuf->offset + hdr_len;

    /* 1. Look for connected socket match */
    socket_t* sock = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        socket_t* s = s_tcp_sockets[i];
        if (s && s->local_port == dst_port && s->remote_port == src_port && s->remote_ip == ip->src_ip) {
            sock = s;
            break;
        }
    }

    /* 2. Look for listening socket match if not connected */
    if (!sock) {
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            socket_t* s = s_tcp_sockets[i];
            if (s && s->local_port == dst_port && s->state == SS_LISTENING) {
                sock = s;
                break;
            }
        }
    }

    if (!sock) {
        /* Send RST if not listening or connected */
        if (!(flags & TCP_RST)) {
            pbuf_t* rst = pbuf_alloc();
            if (rst) {
                struct tcp_hdr* r = (struct tcp_hdr*) rst->payload;
                r->src_port = htons(dst_port);
                r->dst_port = htons(src_port);
                r->seq = htonl(flags & TCP_ACK ? ack : 0);
                r->ack = htonl(seq + payload_len + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0));
                r->data_offset = (5 << 4);
                r->flags = TCP_RST | (flags & TCP_ACK ? 0 : TCP_ACK);
                r->window = 0;
                r->checksum = 0;
                r->urgent_ptr = 0;
                r->checksum = net_checksum_pseudo(r, sizeof(struct tcp_hdr), ip->dst_ip, ip->src_ip, IPPROTO_TCP);
                rst->len = sizeof(struct tcp_hdr);
                rst->offset = 0;
                ipv4_send(rst, ip->src_ip, IPPROTO_TCP);
                pbuf_free(rst);
            }
        }
        pbuf_free(pbuf);
        return;
    }

    kmutex_lock(&sock->lock);
    tcp_cb_t* tcb = (tcp_cb_t*) sock->inet_data;

    /* Listening socket receiving SYN */
    if (sock->state == SS_LISTENING && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
        if (sock->accept_count < sock->accept_backlog) {
            /* Find slot in s_tcp_sockets for child */
            int child_slot = -1;
            for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                if (!s_tcp_sockets[i]) {
                    child_slot = i;
                    break;
                }
            }
            if (child_slot >= 0) {
                socket_t* child = socket_alloc(AF_INET, SOCK_STREAM, 0);
                if (child) {
                    child->local_ip = ip->dst_ip;
                    child->local_port = dst_port;
                    child->remote_ip = ip->src_ip;
                    child->remote_port = src_port;
                    child->ops = &tcp_socket_ops;
                    s_tcp_sockets[child_slot] = child;

                    tcp_cb_t* c_tcb = get_or_create_tcb(child);
                    if (c_tcb) {
                        c_tcb->parent = sock;
                        c_tcb->irs = seq;
                        c_tcb->rcv_nxt = seq + 1;
                        c_tcb->iss = timer_get_ticks() * 1000 + (uint32_t)(uintptr_t)child;
                        c_tcb->snd_nxt = c_tcb->iss;
                        c_tcb->snd_una = c_tcb->iss;
                        c_tcb->state = TCP_STATE_SYN_RECEIVED;

                        tcp_send_segment(child, TCP_SYN | TCP_ACK, c_tcb->snd_nxt, c_tcb->rcv_nxt, NULL, 0);
                        c_tcb->snd_nxt++;
                    }
                }
            }
        }
        kmutex_unlock(&sock->lock);
        pbuf_free(pbuf);
        return;
    }

    if (!tcb) {
        kmutex_unlock(&sock->lock);
        pbuf_free(pbuf);
        return;
    }

    /* Handshake processing */
    if (tcb->state == TCP_STATE_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            tcb->irs = seq;
            tcb->rcv_nxt = seq + 1;
            tcb->snd_una = ack;
            tcb->state = TCP_STATE_ESTABLISHED;
            sock->state = SS_CONNECTED;

            tcp_send_segment(sock, TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            scheduler_wake_channel(&tcb->connect_wait);
        }
        kmutex_unlock(&sock->lock);
        pbuf_free(pbuf);
        return;
    }

    if (tcb->state == TCP_STATE_SYN_RECEIVED) {
        if (flags & TCP_ACK) {
            tcb->snd_una = ack;
            tcb->state = TCP_STATE_ESTABLISHED;
            sock->state = SS_CONNECTED;

            if (tcb->parent) {
                kmutex_lock(&tcb->parent->lock);
                if (tcb->parent->accept_count < UNIX_BACKLOG_MAX) {
                    tcb->parent->accept_queue[tcb->parent->accept_tail] = sock;
                    tcb->parent->accept_tail = (tcb->parent->accept_tail + 1) % UNIX_BACKLOG_MAX;
                    tcb->parent->accept_count++;
                    scheduler_wake_channel(&tcb->parent->accept_wait);
                }
                kmutex_unlock(&tcb->parent->lock);
            }
        }
        kmutex_unlock(&sock->lock);
        pbuf_free(pbuf);
        return;
    }

    /* Data and ACK processing in ESTABLISHED or teardown */
    if (flags & TCP_ACK) {
        if (ack > tcb->snd_una) {
            tcb->snd_una = ack;
            scheduler_wake_channel(&sock->rx_ring.write_wait);
        }
    }

    if (tcb->state == TCP_STATE_ESTABLISHED || tcb->state == TCP_STATE_FIN_WAIT_1 || tcb->state == TCP_STATE_FIN_WAIT_2) {
        if (payload_len > 0) {
            if (seq == tcb->rcv_nxt) {
                for (uint32_t i = 0; i < payload_len; i++) {
                    if (tcb->rx_count < TCP_RX_BUF_SIZE) {
                        tcb->rx_buf[tcb->rx_tail] = payload[i];
                        tcb->rx_tail = (tcb->rx_tail + 1) % TCP_RX_BUF_SIZE;
                        tcb->rx_count++;
                    }
                }
                tcb->rcv_nxt += payload_len;
                scheduler_wake_channel(&sock->rx_ring.read_wait);
                /* Send ACK */
                tcp_send_segment(sock, TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            }
        }

        if (flags & TCP_FIN) {
            tcb->rcv_nxt++;
            tcp_send_segment(sock, TCP_ACK, tcb->snd_nxt, tcb->rcv_nxt, NULL, 0);
            if (tcb->state == TCP_STATE_ESTABLISHED) {
                tcb->state = TCP_STATE_CLOSE_WAIT;
            } else if (tcb->state == TCP_STATE_FIN_WAIT_1 || tcb->state == TCP_STATE_FIN_WAIT_2) {
                tcb->state = TCP_STATE_TIME_WAIT;
            }
            sock->shutdown_rd = 1;
            scheduler_wake_channel(&sock->rx_ring.read_wait);
        }
    }

    kmutex_unlock(&sock->lock);
    pbuf_free(pbuf);
}

int tcp_get_table_text(char* buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t off = 0;

    int n = ksnprintf(buf + off, max_len - off,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    if (n > 0) off += (size_t)n;

    int sl = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        socket_t* s = s_tcp_sockets[i];
        if (!s) continue;

        tcp_cb_t* tcb = (tcp_cb_t*) s->inet_data;
        int st = tcb ? tcb->state : 0;
        char loc[32], rem[32];
        ksnprintf(loc, sizeof(loc), "%08X:%04X", s->local_ip, s->local_port);
        ksnprintf(rem, sizeof(rem), "%08X:%04X", s->remote_ip, s->remote_port);

        n = ksnprintf(buf + off, max_len - off,
            "%4d: %-13s %-13s %02X 00000000:%08X 00:00000000 00000000     0        0 %u 1 0\n",
            sl++, loc, rem, st, tcb ? tcb->rx_count : 0, s->inode);
        if (n > 0) off += (size_t)n;
    }

    return (int)off;
}

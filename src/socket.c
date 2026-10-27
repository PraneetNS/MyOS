#include "socket.h"
#include "kheap.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
#include "tmpfs.h"
#include "serial.h"
#include "vga.h"
#include "errno.h"

static socket_t* all_sockets_head = NULL;
static uint32_t next_sock_inode = 10001;

static uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint32_t flags) {
    asm volatile ("pushl %0; popfl" :: "r"(flags) : "memory");
}

static void kstrncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    while (i + 1 < n && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (n > 0) dst[i] = '\0';
}

static void unix_ring_init(unix_ring_t* r) {
    r->buffer = (uint8_t*) kmalloc(UNIX_RING_SIZE);
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static void unix_ring_destroy(unix_ring_t* r) {
    if (r->buffer) {
        kfree(r->buffer);
        r->buffer = NULL;
    }
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

void socket_init(void) {
    all_sockets_head = NULL;
}

socket_t* socket_alloc(int domain, int type, int protocol) {
    socket_t* s = (socket_t*) kmalloc(sizeof(socket_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(socket_t));

    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->state = SS_UNCONNECTED;
    s->refcount = 1;
    kmutex_init(&s->lock, "socket");

    process_t* me = scheduler_current();
    s->creator_pid = me ? me->pid : 0;
    s->creator_agent_id = me ? me->agent_id : 0;

    uint32_t f = irq_save();
    s->inode = next_sock_inode++;
    s->next_all = all_sockets_head;
    if (all_sockets_head) {
        all_sockets_head->prev_all = s;
    }
    all_sockets_head = s;
    irq_restore(f);

    return s;
}

void socket_ref(socket_t* s) {
    if (!s) return;
    uint32_t f = irq_save();
    s->refcount++;
    irq_restore(f);
}

void socket_close(socket_t* s) {
    if (!s) return;

    socket_t* peer_to_unref = NULL;
    int unref_self = 0;

    uint32_t f = irq_save();
    s->shutdown_rd = 1;
    s->shutdown_wr = 1;
    if (s->peer) {
        socket_t* peer = s->peer;
        s->peer = NULL;
        peer->peer = NULL;
        peer->peer_closed = 1;
        scheduler_wake_channel(&peer->rx_ring.read_wait);
        scheduler_wake_channel(&peer->rx_ring.write_wait);
        peer_to_unref = peer;
        unref_self = 1;
    }
    irq_restore(f);

    if (peer_to_unref) socket_unref(peer_to_unref);
    if (unref_self) socket_unref(s);
}

void socket_unref(socket_t* s) {
    if (!s) return;
    uint32_t f = irq_save();
    s->refcount--;
    if (s->refcount > 0) {
        irq_restore(f);
        return;
    }

    /* Remove from global list */
    if (s->prev_all) {
        s->prev_all->next_all = s->next_all;
    } else if (all_sockets_head == s) {
        all_sockets_head = s->next_all;
    }
    if (s->next_all) {
        s->next_all->prev_all = s->prev_all;
    }
    irq_restore(f);

    /* Disconnect peer if connected */
    if (s->peer) {
        socket_t* peer = s->peer;
        s->peer = NULL;
        peer->peer = NULL;
        peer->peer_closed = 1;
        scheduler_wake_channel(&peer->rx_ring.read_wait);
        scheduler_wake_channel(&peer->rx_ring.write_wait);
        socket_unref(peer);
    }

    /* Free bound vnode if any */
    if (s->bound_vn) {
        vnode_unref(s->bound_vn);
        s->bound_vn = NULL;
    }

    /* Free buffered data */
    unix_ring_destroy(&s->rx_ring);

    /* Clean up any unaccepted sockets */
    while (s->accept_count > 0) {
        socket_t* pending = s->accept_queue[s->accept_head];
        s->accept_head = (s->accept_head + 1) % UNIX_BACKLOG_MAX;
        s->accept_count--;
        if (pending) socket_unref(pending);
    }

    kfree(s);
}

int socket_bind(socket_t* s, const struct sockaddr* addr, uint32_t addrlen) {
    if (!s || !addr) return -EINVAL;
    if (addr->sa_family != AF_UNIX) return -EAFNOSUPPORT;
    if (addrlen < sizeof(sa_family_t) + 1) return -EINVAL;

    const struct sockaddr_un* un = (const struct sockaddr_un*) addr;
    if (un->sun_path[0] == '\0') return -EINVAL; /* abstract namespace not supported */

    kmutex_lock(&s->lock);
    if (s->state != SS_UNCONNECTED || s->bound_vn != NULL) {
        kmutex_unlock(&s->lock);
        return -EINVAL;
    }

    process_t* me = scheduler_current();
    const char* cwd = me ? me->cwd : "/";

    vnode_t* dir = NULL;
    char name[NAME_MAX + 1];
    int err = vfs_resolve_parent(un->sun_path, cwd, &dir, name, sizeof(name));
    if (err != 0 || !dir) {
        kmutex_unlock(&s->lock);
        return (err != 0) ? err : -ENOENT;
    }

    if (!tmpfs_is_tmpfs_vnode(dir)) {
        vnode_unref(dir);
        kmutex_unlock(&s->lock);
        return -EOPNOTSUPP;
    }

    vnode_t* sock_vn = NULL;
    err = tmpfs_create_socket_node(dir, name, s, &sock_vn);
    vnode_unref(dir);

    if (err != 0) {
        kmutex_unlock(&s->lock);
        return err;
    }

    s->bound_vn = sock_vn;
    vfs_path_canonical(un->sun_path, cwd, s->sun_path, sizeof(s->sun_path));

    kmutex_unlock(&s->lock);
    return 0;
}

int socket_listen(socket_t* s, int backlog) {
    if (!s) return -EINVAL;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;

    kmutex_lock(&s->lock);
    if (s->state != SS_UNCONNECTED || !s->bound_vn) {
        kmutex_unlock(&s->lock);
        return -EINVAL;
    }

    s->state = SS_LISTENING;
    s->accept_head = 0;
    s->accept_tail = 0;
    s->accept_count = 0;
    s->accept_backlog = (backlog > 0 && backlog <= UNIX_BACKLOG_MAX) ? backlog : (backlog <= 0 ? 5 : UNIX_BACKLOG_MAX);

    kmutex_unlock(&s->lock);
    return 0;
}

int socket_accept(socket_t* s, struct sockaddr* addr, uint32_t* addrlen, int flags, socket_t** out) {
    if (!s || !out) return -EINVAL;
    if (s->state != SS_LISTENING) return -EINVAL;

    int nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;
    process_t* me = scheduler_current();
    if (me && (me->current_op_flags & (O_NONBLOCK | O_NONBLOCK_LINUX))) {
        nonblock = 1;
    }

    while (s->accept_count == 0) {
        if (s->state != SS_LISTENING) return -EINVAL;
        if (nonblock) return -EAGAIN;
        if (signal_has_deliverable(scheduler_current())) return -EINTR;
        scheduler_wait_channel(&s->accept_wait);
    }

    uint32_t f = irq_save();
    if (s->accept_count == 0) {
        irq_restore(f);
        return -EAGAIN;
    }

    socket_t* conn = s->accept_queue[s->accept_head];
    s->accept_queue[s->accept_head] = NULL;
    s->accept_head = (s->accept_head + 1) % UNIX_BACKLOG_MAX;
    s->accept_count--;
    irq_restore(f);

    if (addr && addrlen) {
        struct sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        kstrncpy(un.sun_path, conn->sun_path, sizeof(un.sun_path));
        uint32_t to_copy = sizeof(un);
        if (*addrlen < to_copy) to_copy = *addrlen;
        memcpy(addr, &un, to_copy);
        *addrlen = to_copy;
    }

    *out = conn;
    return 0;
}

int socket_connect(socket_t* s, const struct sockaddr* addr, uint32_t addrlen) {
    if (!s || !addr) return -EINVAL;
    if (addr->sa_family != AF_UNIX) return -EAFNOSUPPORT;
    if (addrlen < sizeof(sa_family_t) + 1) return -EINVAL;

    const struct sockaddr_un* un = (const struct sockaddr_un*) addr;
    if (un->sun_path[0] == '\0') return -EINVAL;

    kmutex_lock(&s->lock);
    if (s->state != SS_UNCONNECTED) {
        kmutex_unlock(&s->lock);
        return (s->state == SS_CONNECTED) ? -EISCONN : -EINVAL;
    }

    process_t* me = scheduler_current();
    const char* cwd = me ? me->cwd : "/";

    vnode_t* srv_vn = NULL;
    int err = vfs_resolve_path(un->sun_path, cwd, &srv_vn);
    if (err != 0 || !srv_vn) {
        kmutex_unlock(&s->lock);
        return -ENOENT;
    }

    if (!tmpfs_is_tmpfs_vnode(srv_vn)) {
        vnode_unref(srv_vn);
        kmutex_unlock(&s->lock);
        return -ECONNREFUSED;
    }

    tmpfs_node_t* tn = (tmpfs_node_t*) srv_vn->fs_data;
    if (!tn || tn->type != TMPFS_NODE_SOCK || !tn->sock_data) {
        vnode_unref(srv_vn);
        kmutex_unlock(&s->lock);
        return -ECONNREFUSED;
    }

    socket_t* srv = (socket_t*) tn->sock_data;
    if (srv->state != SS_LISTENING) {
        vnode_unref(srv_vn);
        kmutex_unlock(&s->lock);
        return -ECONNREFUSED;
    }

    if (srv->accept_count >= srv->accept_backlog) {
        vnode_unref(srv_vn);
        kmutex_unlock(&s->lock);
        return -ECONNREFUSED;
    }

    /* Allocate server-side connection socket */
    socket_t* srv_conn = socket_alloc(AF_UNIX, SOCK_STREAM, 0);
    if (!srv_conn) {
        vnode_unref(srv_vn);
        kmutex_unlock(&s->lock);
        return -ENOMEM;
    }

    unix_ring_init(&s->rx_ring);
    unix_ring_init(&srv_conn->rx_ring);

    kstrncpy(s->peer_path, srv->sun_path, sizeof(s->peer_path));
    kstrncpy(srv_conn->sun_path, s->sun_path, sizeof(srv_conn->sun_path));
    kstrncpy(srv_conn->peer_path, s->sun_path, sizeof(srv_conn->peer_path));

    s->peer = srv_conn;
    socket_ref(srv_conn);
    srv_conn->peer = s;
    socket_ref(s);

    s->peer_pid = srv->creator_pid;
    s->peer_agent_id = srv->creator_agent_id;
    srv_conn->peer_pid = me ? me->pid : 0;
    srv_conn->peer_agent_id = me ? me->agent_id : 0;

    s->state = SS_CONNECTED;
    srv_conn->state = SS_CONNECTED;

    /* Enqueue to server */
    uint32_t f = irq_save();
    srv->accept_queue[srv->accept_tail] = srv_conn;
    srv->accept_tail = (srv->accept_tail + 1) % UNIX_BACKLOG_MAX;
    srv->accept_count++;
    irq_restore(f);

    scheduler_wake_channel(&srv->accept_wait);
    vnode_unref(srv_vn);

    kmutex_unlock(&s->lock);
    return 0;
}

int socket_send(socket_t* s, const void* buf, uint32_t len, int flags) {
    if (!s || (!buf && len > 0)) return -EINVAL;
    if (len == 0) return 0;

    int nonblock = (flags & MSG_DONTWAIT) ? 1 : 0;
    process_t* me = scheduler_current();
    if (me && (me->current_op_flags & (O_NONBLOCK | O_NONBLOCK_LINUX))) {
        nonblock = 1;
    }

    if (s->state != SS_CONNECTED || !s->peer || s->shutdown_wr || s->peer_closed) {
        if (!(flags & MSG_NOSIGNAL) && me) {
            sig_send(me, SIGPIPE);
        }
        return -EPIPE;
    }

    socket_t* peer = s->peer;
    const uint8_t* src = (const uint8_t*) buf;
    uint32_t written = 0;

    while (written < len) {
        if (!s->peer || s->peer_closed || s->shutdown_wr) {
            if (!(flags & MSG_NOSIGNAL) && me) {
                sig_send(me, SIGPIPE);
            }
            return (written > 0) ? (int) written : -EPIPE;
        }

        while (peer->rx_ring.count == UNIX_RING_SIZE) {
            if (!s->peer || s->peer_closed || s->shutdown_wr) {
                if (!(flags & MSG_NOSIGNAL) && me) {
                    sig_send(me, SIGPIPE);
                }
                return (written > 0) ? (int) written : -EPIPE;
            }
            if (nonblock) {
                return (written > 0) ? (int) written : -EAGAIN;
            }
            if (signal_has_deliverable(scheduler_current())) {
                return (written > 0) ? (int) written : -EINTR;
            }
            scheduler_wait_channel(&peer->rx_ring.write_wait);
        }

        uint32_t f = irq_save();
        while (written < len && peer->rx_ring.count < UNIX_RING_SIZE) {
            peer->rx_ring.buffer[peer->rx_ring.head] = src[written++];
            peer->rx_ring.head = (peer->rx_ring.head + 1) % UNIX_RING_SIZE;
            peer->rx_ring.count++;
        }
        irq_restore(f);

        scheduler_wake_channel(&peer->rx_ring.read_wait);
    }

    return (int) written;
}

int socket_recv(socket_t* s, void* buf, uint32_t len, int flags) {
    if (!s || (!buf && len > 0)) return -EINVAL;
    if (len == 0) return 0;
    if (s->shutdown_rd) return 0;

    int nonblock = (flags & MSG_DONTWAIT) ? 1 : 0;
    process_t* me = scheduler_current();
    if (me && (me->current_op_flags & (O_NONBLOCK | O_NONBLOCK_LINUX))) {
        nonblock = 1;
    }

    uint8_t* dst = (uint8_t*) buf;
    uint32_t read_bytes = 0;

    while (s->rx_ring.count == 0) {
        if (s->peer_closed || s->peer_shutdown_wr || !s->peer) {
            return 0; /* EOF */
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
        scheduler_wait_channel(&s->rx_ring.read_wait);
    }

    uint32_t f = irq_save();
    while (read_bytes < len && s->rx_ring.count > 0) {
        dst[read_bytes++] = s->rx_ring.buffer[s->rx_ring.tail];
        s->rx_ring.tail = (s->rx_ring.tail + 1) % UNIX_RING_SIZE;
        s->rx_ring.count--;
    }
    irq_restore(f);

    scheduler_wake_channel(&s->rx_ring.write_wait);
    return (int) read_bytes;
}

int socket_shutdown(socket_t* s, int how) {
    if (!s) return -EINVAL;
    if (s->state != SS_CONNECTED) return -ENOTCONN;

    if (how == SHUT_RD || how == SHUT_RDWR) {
        s->shutdown_rd = 1;
        scheduler_wake_channel(&s->rx_ring.read_wait);
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        s->shutdown_wr = 1;
        if (s->peer) {
            s->peer->peer_shutdown_wr = 1;
            scheduler_wake_channel(&s->peer->rx_ring.read_wait);
        }
    }
    return 0;
}

short socket_poll(socket_t* s, short events, void** chan_out) {
    if (!s) return POLLNVAL;
    short revents = 0;

    if (s->state == SS_LISTENING) {
        if (s->accept_count > 0) {
            if (events & POLLIN) revents |= POLLIN;
        } else if ((events & POLLIN) && chan_out && !*chan_out) {
            *chan_out = &s->accept_wait;
        }
        return revents;
    }

    if (s->state == SS_CONNECTED) {
        if (s->rx_ring.count > 0 || s->shutdown_rd || s->peer_closed || s->peer_shutdown_wr) {
            if (events & POLLIN) revents |= POLLIN;
        } else if ((events & POLLIN) && chan_out && !*chan_out) {
            *chan_out = &s->rx_ring.read_wait;
        }

        if (!s->shutdown_wr && s->peer && !s->peer_closed) {
            if ((events & POLLOUT) && s->peer->rx_ring.count < UNIX_RING_SIZE) {
                revents |= POLLOUT;
            } else if ((events & POLLOUT) && chan_out && !*chan_out) {
                *chan_out = &s->peer->rx_ring.write_wait;
            }
        }

        if (s->peer_closed) {
            revents |= POLLHUP;
        }
        return revents;
    }

    return POLLHUP;
}

int socket_pair(int domain, int type, int protocol, socket_t** s1_out, socket_t** s2_out) {
    if (domain != AF_UNIX || type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!s1_out || !s2_out) return -EINVAL;

    socket_t* s1 = socket_alloc(domain, type, protocol);
    socket_t* s2 = socket_alloc(domain, type, protocol);
    if (!s1 || !s2) {
        if (s1) socket_unref(s1);
        if (s2) socket_unref(s2);
        return -ENOMEM;
    }

    unix_ring_init(&s1->rx_ring);
    unix_ring_init(&s2->rx_ring);

    s1->peer = s2; socket_ref(s2);
    s2->peer = s1; socket_ref(s1);

    process_t* me = scheduler_current();
    s1->peer_pid = me ? me->pid : 0;
    s1->peer_agent_id = me ? me->agent_id : 0;
    s2->peer_pid = me ? me->pid : 0;
    s2->peer_agent_id = me ? me->agent_id : 0;

    s1->state = SS_CONNECTED;
    s2->state = SS_CONNECTED;

    *s1_out = s1;
    *s2_out = s2;
    return 0;
}

int socket_get_peerinfo(socket_t* s, int* pid_out, int* agent_out) {
    if (!s) return -EINVAL;
    kmutex_lock(&s->lock);
    if (s->state != SS_CONNECTED) {
        kmutex_unlock(&s->lock);
        return -ENOTCONN;
    }
    if (pid_out) *pid_out = s->peer_pid;
    if (agent_out) *agent_out = s->peer_agent_id;
    kmutex_unlock(&s->lock);
    return 0;
}

int socket_getsockname(socket_t* s, struct sockaddr* addr, uint32_t* addrlen) {
    if (!s || !addr || !addrlen) return -EINVAL;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    kstrncpy(un.sun_path, s->sun_path, sizeof(un.sun_path));
    uint32_t to_copy = sizeof(un);
    if (*addrlen < to_copy) to_copy = *addrlen;
    memcpy(addr, &un, to_copy);
    *addrlen = to_copy;
    return 0;
}

int socket_getpeername(socket_t* s, struct sockaddr* addr, uint32_t* addrlen) {
    if (!s || !addr || !addrlen) return -EINVAL;
    if (s->state != SS_CONNECTED || s->peer_closed) return -ENOTCONN;
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    kstrncpy(un.sun_path, s->peer_path, sizeof(un.sun_path));
    uint32_t to_copy = sizeof(un);
    if (*addrlen < to_copy) to_copy = *addrlen;
    memcpy(addr, &un, to_copy);
    *addrlen = to_copy;
    return 0;
}

int socket_stat(socket_t* s, struct stat* st) {
    if (!s || !st) return -EINVAL;
    memset(st, 0, sizeof(struct stat));
    st->st_ino = s->inode;
    st->st_mode = S_IFSOCK | 0777;
    st->st_nlink = 1;
    st->st_size = s->rx_ring.count;
    st->st_blksize = 512;
    return 0;
}

socket_t* socket_get_list_head(void) {
    return all_sockets_head;
}

#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "kmutex.h"

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    1
#define AF_INET     2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOCK_NONBLOCK 0x4000
#define SOCK_CLOEXEC  0x80000

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000

#define UNIX_PATH_MAX   108
#define UNIX_RING_SIZE  (64 * 1024)
#define UNIX_BACKLOG_MAX 32

typedef unsigned short sa_family_t;
typedef uint32_t socklen_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[UNIX_PATH_MAX];
};

typedef enum {
    SS_FREE = 0,
    SS_UNCONNECTED,
    SS_LISTENING,
    SS_CONNECTED,
    SS_CLOSED
} socket_state_t;

typedef struct unix_ring {
    uint8_t* buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int read_wait;
    int write_wait;
} unix_ring_t;

struct socket;

struct socket_ops {
    int (*bind)(struct socket* sock, const struct sockaddr* addr, uint32_t addrlen);
    int (*listen)(struct socket* sock, int backlog);
    int (*accept)(struct socket* sock, struct sockaddr* addr, uint32_t* addrlen, int flags, struct socket** out);
    int (*connect)(struct socket* sock, const struct sockaddr* addr, uint32_t addrlen);
    int (*send)(struct socket* sock, const void* buf, uint32_t len, int flags);
    int (*recv)(struct socket* sock, void* buf, uint32_t len, int flags);
    int (*shutdown)(struct socket* sock, int how);
    short (*poll)(struct socket* sock, short events, void** chan_out);
    void (*close)(struct socket* sock);
    int (*getsockname)(struct socket* sock, struct sockaddr* addr, uint32_t* addrlen);
    int (*getpeername)(struct socket* sock, struct sockaddr* addr, uint32_t* addrlen);
};

typedef struct socket {
    int domain;
    int type;
    int protocol;
    socket_state_t state;
    int flags;
    int refcount;
    uint32_t inode;
    const struct socket_ops* ops;
    kmutex_t lock;

    /* AF_UNIX specific */
    char sun_path[UNIX_PATH_MAX];
    char peer_path[UNIX_PATH_MAX];
    vnode_t* bound_vn;

    /* Connection */
    struct socket* peer;
    int peer_closed;
    int shutdown_rd;
    int shutdown_wr;
    int peer_shutdown_wr;

    /* Buffer for incoming data on this socket */
    unix_ring_t rx_ring;

    /* Listening / accept queue */
    struct socket* accept_queue[UNIX_BACKLOG_MAX];
    int accept_head;
    int accept_tail;
    int accept_count;
    int accept_backlog;
    int accept_wait;

    struct socket* next_all;
    struct socket* prev_all;
} socket_t;

void socket_init(void);
socket_t* socket_alloc(int domain, int type, int protocol);
void socket_ref(socket_t* s);
void socket_unref(socket_t* s);

int socket_bind(socket_t* s, const struct sockaddr* addr, uint32_t addrlen);
int socket_listen(socket_t* s, int backlog);
int socket_accept(socket_t* s, struct sockaddr* addr, uint32_t* addrlen, int flags, socket_t** out);
int socket_connect(socket_t* s, const struct sockaddr* addr, uint32_t addrlen);
int socket_send(socket_t* s, const void* buf, uint32_t len, int flags);
int socket_recv(socket_t* s, void* buf, uint32_t len, int flags);
int socket_shutdown(socket_t* s, int how);
short socket_poll(socket_t* s, short events, void** chan_out);
int socket_pair(int domain, int type, int protocol, socket_t** s1_out, socket_t** s2_out);
int socket_getsockname(socket_t* s, struct sockaddr* addr, uint32_t* addrlen);
int socket_getpeername(socket_t* s, struct sockaddr* addr, uint32_t* addrlen);
int socket_stat(socket_t* s, struct stat* st);
void socket_close(socket_t* s);

socket_t* socket_get_list_head(void);

#endif

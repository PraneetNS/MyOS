#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "tty.h"
#include "kmutex.h"

#define MAX_PTYS 16

#define PTY_BUF_SIZE 4096

typedef struct pty_ring {
    uint8_t buffer[PTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int read_wait;
    int write_wait;
} pty_ring_t;

typedef struct pty {
    int in_use;
    int index;
    int locked;
    int master_open;
    int slave_open_count;

    /* Ring buffer for data written by slave, read by master */
    pty_ring_t master_in;

    /* Slave line discipline */
    tty_t slave_tty;

    /* Associated VFS vnodes */
    vnode_t master_vn;
    vnode_t slave_vn;

    kmutex_t lock;
} pty_t;

void pty_init(void);
int pty_create(vnode_t** out_master_vn);
vnode_t* pty_get_slave_vnode(int index);
int pty_is_ptmx_vnode(vnode_t* vn);
int pty_is_slave_vnode(vnode_t* vn, int* out_index);
int pty_count_active(void);
int pty_get_active_index(int slot);
void pty_master_close(pty_t* pty);
void pty_slave_close(pty_t* pty);

#endif /* PTY_H */

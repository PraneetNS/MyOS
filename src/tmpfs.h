#ifndef TMPFS_H
#define TMPFS_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "kmutex.h"

#define TMPFS_MAX_BLOCKS 16
#define TMPFS_PAGES_PER_BLOCK 1024 /* 1024 * 4KB = 4MB per block, up to 64MB per file */

typedef enum {
    TMPFS_NODE_REG = 1,
    TMPFS_NODE_DIR,
    TMPFS_NODE_SOCK
} tmpfs_node_type_t;

typedef struct tmpfs_dirent {
    char name[NAME_MAX + 1];
    struct tmpfs_node* node;
    struct tmpfs_dirent* next;
} tmpfs_dirent_t;

typedef struct tmpfs_fs {
    uint32_t used_frames;
    uint32_t max_frames;
    kmutex_t lock;
    struct tmpfs_node* root;
} tmpfs_fs_t;

typedef struct tmpfs_node {
    tmpfs_node_type_t type;
    uint32_t size;
    uint32_t mode;
    uint32_t ino;
    int refcount;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;

    /* Directory entries (if type == TMPFS_NODE_DIR) */
    tmpfs_dirent_t* children;

    /* Sparse frame array: two-level table (if type == TMPFS_NODE_REG) */
    uint32_t* page_blocks[TMPFS_MAX_BLOCKS];

    /* Socket pointer (if type == TMPFS_NODE_SOCK) */
    void* sock_data;

    /* Owning filesystem instance */
    tmpfs_fs_t* fs;
} tmpfs_node_t;

void tmpfs_init(void);
int tmpfs_mount(const char* mount_path);
uint32_t tmpfs_get_or_alloc_frame(tmpfs_node_t* node, uint32_t page_idx, int allocate, int* err_out);
uint32_t tmpfs_get_frame(tmpfs_node_t* node, uint32_t page_idx);
void tmpfs_node_ref(tmpfs_node_t* node);
void tmpfs_node_unref(tmpfs_node_t* node);

extern const struct fs_ops tmpfs_ops;

#endif /* TMPFS_H */

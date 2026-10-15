#include "tmpfs.h"
#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "rtc.h"
#include "serial.h"
#include "vfs.h"

static int tmpfs_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static char* tmpfs_strncpy(char* dest, const char* src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

#define strcmp tmpfs_strcmp
#define strncpy tmpfs_strncpy

static uint32_t next_tmpfs_ino = 1000;

static void tmpfs_free_page_blocks(tmpfs_node_t* node) {
    if (!node || !node->fs) return;
    for (int b = 0; b < TMPFS_MAX_BLOCKS; b++) {
        if (node->page_blocks[b]) {
            for (int p = 0; p < TMPFS_PAGES_PER_BLOCK; p++) {
                uint32_t phys = node->page_blocks[b][p];
                if (phys != 0) {
                    pmm_unref(phys);
                    if (node->fs->used_frames > 0) {
                        node->fs->used_frames--;
                    }
                    node->page_blocks[b][p] = 0;
                }
            }
            kfree(node->page_blocks[b]);
            node->page_blocks[b] = NULL;
        }
    }
}

void tmpfs_node_ref(tmpfs_node_t* node) {
    if (node) {
        node->refcount++;
    }
}

void tmpfs_node_unref(tmpfs_node_t* node) {
    if (!node) return;
    node->refcount--;
    if (node->refcount <= 0) {
        if (node->type == TMPFS_NODE_REG) {
            tmpfs_free_page_blocks(node);
        } else if (node->type == TMPFS_NODE_DIR) {
            tmpfs_dirent_t* curr = node->children;
            while (curr) {
                tmpfs_dirent_t* next = curr->next;
                tmpfs_node_unref(curr->node);
                kfree(curr);
                curr = next;
            }
            node->children = NULL;
        }
        kfree(node);
    }
}

uint32_t tmpfs_get_or_alloc_frame(tmpfs_node_t* node, uint32_t page_idx, int allocate, int* err_out) {
    if (!node || !node->fs) {
        if (err_out) *err_out = -EINVAL;
        return 0;
    }

    uint32_t block_idx = page_idx / TMPFS_PAGES_PER_BLOCK;
    uint32_t page_in_block = page_idx % TMPFS_PAGES_PER_BLOCK;

    if (block_idx >= TMPFS_MAX_BLOCKS) {
        if (err_out) *err_out = -EFBIG;
        return 0;
    }

    if (!node->page_blocks[block_idx]) {
        if (!allocate) {
            if (err_out) *err_out = 0;
            return 0; /* Hole */
        }
        uint32_t* blk = (uint32_t*) kmalloc(TMPFS_PAGES_PER_BLOCK * sizeof(uint32_t));
        if (!blk) {
            if (err_out) *err_out = -ENOMEM;
            return 0;
        }
        memset(blk, 0, TMPFS_PAGES_PER_BLOCK * sizeof(uint32_t));
        node->page_blocks[block_idx] = blk;
    }

    uint32_t phys = node->page_blocks[block_idx][page_in_block];
    if (phys != 0) {
        if (err_out) *err_out = 0;
        return phys;
    }

    if (!allocate) {
        if (err_out) *err_out = 0;
        return 0; /* Hole */
    }

    /* Check size cap */
    if (node->fs->used_frames >= node->fs->max_frames) {
        if (err_out) *err_out = -ENOSPC;
        return 0;
    }

    phys = pmm_alloc_frame();
    if (phys == 0) {
        if (err_out) *err_out = -ENOSPC;
        return 0;
    }

    /* Zero the newly allocated page */
    memset(P2V(phys), 0, FRAME_SIZE);
    node->page_blocks[block_idx][page_in_block] = phys;
    node->fs->used_frames++;

    if (err_out) *err_out = 0;
    return phys;
}

uint32_t tmpfs_get_frame(tmpfs_node_t* node, uint32_t page_idx) {
    return tmpfs_get_or_alloc_frame(node, page_idx, 0, NULL);
}

static tmpfs_node_t* tmpfs_alloc_node(tmpfs_fs_t* fs, tmpfs_node_type_t type, uint32_t mode) {
    tmpfs_node_t* node = (tmpfs_node_t*) kmalloc(sizeof(tmpfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(tmpfs_node_t));

    node->type = type;
    node->mode = mode;
    node->ino = next_tmpfs_ino++;
    node->refcount = 1;
    node->fs = fs;

    uint32_t now = (uint32_t) rtc_get_boot_epoch();
    node->atime = now;
    node->mtime = now;
    node->ctime = now;

    return node;
}

/* VFS Operations */

static int tmpfs_op_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    if (!dir || !dir->fs_data || !name || !out) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    if (strcmp(name, ".") == 0) {
        *out = dir;
        vnode_ref(dir);
        kmutex_unlock(&fs->lock);
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        vnode_t* parent = dir->parent ? dir->parent : dir;
        *out = parent;
        vnode_ref(parent);
        kmutex_unlock(&fs->lock);
        return 0;
    }

    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            vnode_type_t vn_type = (curr->node->type == TMPFS_NODE_DIR) ? VNODE_DIR : VNODE_FILE;
            vnode_t* vn = vnode_alloc(vn_type, &tmpfs_ops, curr->node);
            if (!vn) {
                kmutex_unlock(&fs->lock);
                return -ENOMEM;
            }
            tmpfs_node_ref(curr->node);
            vn->parent = dir;
            *out = vn;
            kmutex_unlock(&fs->lock);
            return 0;
        }
        curr = curr->next;
    }

    kmutex_unlock(&fs->lock);
    return -ENOENT;
}

static int tmpfs_op_read(vnode_t* node, uint32_t offset, uint8_t* buf, uint32_t count) {
    if (!node || !node->fs_data || !buf) return -EINVAL;
    tmpfs_node_t* tnode = (tmpfs_node_t*) node->fs_data;
    tmpfs_fs_t* fs = tnode->fs;

    kmutex_lock(&fs->lock);

    if (offset >= tnode->size) {
        kmutex_unlock(&fs->lock);
        return 0;
    }

    if (offset + count > tnode->size) {
        count = tnode->size - offset;
    }

    uint32_t bytes_read = 0;
    while (bytes_read < count) {
        uint32_t cur_off = offset + bytes_read;
        uint32_t page_idx = cur_off / FRAME_SIZE;
        uint32_t page_off = cur_off % FRAME_SIZE;
        uint32_t chunk = FRAME_SIZE - page_off;
        if (chunk > count - bytes_read) {
            chunk = count - bytes_read;
        }

        uint32_t phys = tmpfs_get_or_alloc_frame(tnode, page_idx, 0, NULL);
        if (phys == 0) {
            /* Sparse hole: read as zeroes */
            memset(buf + bytes_read, 0, chunk);
        } else {
            memcpy(buf + bytes_read, (const uint8_t*)P2V(phys) + page_off, chunk);
        }
        bytes_read += chunk;
    }

    kmutex_unlock(&fs->lock);
    return (int) bytes_read;
}

static int tmpfs_op_write(vnode_t* node, uint32_t offset, const uint8_t* buf, uint32_t count) {
    if (!node || !node->fs_data || !buf) return -EINVAL;
    tmpfs_node_t* tnode = (tmpfs_node_t*) node->fs_data;
    tmpfs_fs_t* fs = tnode->fs;

    kmutex_lock(&fs->lock);

    uint32_t bytes_written = 0;
    while (bytes_written < count) {
        uint32_t cur_off = offset + bytes_written;
        uint32_t page_idx = cur_off / FRAME_SIZE;
        uint32_t page_off = cur_off % FRAME_SIZE;
        uint32_t chunk = FRAME_SIZE - page_off;
        if (chunk > count - bytes_written) {
            chunk = count - bytes_written;
        }

        int err = 0;
        uint32_t phys = tmpfs_get_or_alloc_frame(tnode, page_idx, 1, &err);
        if (phys == 0) {
            kmutex_unlock(&fs->lock);
            if (bytes_written > 0) return (int) bytes_written;
            return err ? err : -ENOSPC;
        }

        memcpy((uint8_t*)P2V(phys) + page_off, buf + bytes_written, chunk);
        bytes_written += chunk;
    }

    if (offset + bytes_written > tnode->size) {
        tnode->size = offset + bytes_written;
        node->size = tnode->size;
    }

    kmutex_unlock(&fs->lock);
    return (int) bytes_written;
}

static int tmpfs_op_create(vnode_t* dir, const char* name, vnode_t** out) {
    if (!dir || !dir->fs_data || !name || !out) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    /* Check if already exists */
    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            kmutex_unlock(&fs->lock);
            return -EEXIST;
        }
        curr = curr->next;
    }

    tmpfs_node_t* new_node = tmpfs_alloc_node(fs, TMPFS_NODE_REG, S_IFREG | 0644);
    if (!new_node) {
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }

    tmpfs_dirent_t* ent = (tmpfs_dirent_t*) kmalloc(sizeof(tmpfs_dirent_t));
    if (!ent) {
        tmpfs_node_unref(new_node);
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }
    strncpy(ent->name, name, NAME_MAX);
    ent->name[NAME_MAX] = '\0';
    ent->node = new_node;
    ent->next = dir_node->children;
    dir_node->children = ent;

    vnode_t* vn = vnode_alloc(VNODE_FILE, &tmpfs_ops, new_node);
    if (!vn) {
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }
    tmpfs_node_ref(new_node);
    vn->parent = dir;
    *out = vn;

    kmutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_op_mkdir(vnode_t* dir, const char* name, vnode_t** out) {
    if (!dir || !dir->fs_data || !name || !out) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            kmutex_unlock(&fs->lock);
            return -EEXIST;
        }
        curr = curr->next;
    }

    tmpfs_node_t* new_node = tmpfs_alloc_node(fs, TMPFS_NODE_DIR, S_IFDIR | 0755);
    if (!new_node) {
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }

    tmpfs_dirent_t* ent = (tmpfs_dirent_t*) kmalloc(sizeof(tmpfs_dirent_t));
    if (!ent) {
        tmpfs_node_unref(new_node);
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }
    strncpy(ent->name, name, NAME_MAX);
    ent->name[NAME_MAX] = '\0';
    ent->node = new_node;
    ent->next = dir_node->children;
    dir_node->children = ent;

    vnode_t* vn = vnode_alloc(VNODE_DIR, &tmpfs_ops, new_node);
    if (!vn) {
        kmutex_unlock(&fs->lock);
        return -ENOMEM;
    }
    tmpfs_node_ref(new_node);
    vn->parent = dir;
    *out = vn;

    kmutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_op_unlink(vnode_t* dir, const char* name) {
    if (!dir || !dir->fs_data || !name) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    tmpfs_dirent_t* prev = NULL;
    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->node->type == TMPFS_NODE_DIR) {
                kmutex_unlock(&fs->lock);
                return -EISDIR;
            }
            if (prev) {
                prev->next = curr->next;
            } else {
                dir_node->children = curr->next;
            }
            tmpfs_node_t* target = curr->node;
            kfree(curr);
            tmpfs_node_unref(target);
            kmutex_unlock(&fs->lock);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    kmutex_unlock(&fs->lock);
    return -ENOENT;
}

static int tmpfs_op_rmdir(vnode_t* dir, const char* name) {
    if (!dir || !dir->fs_data || !name) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    tmpfs_dirent_t* prev = NULL;
    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->node->type != TMPFS_NODE_DIR) {
                kmutex_unlock(&fs->lock);
                return -ENOTDIR;
            }
            if (curr->node->children != NULL) {
                kmutex_unlock(&fs->lock);
                return -ENOTEMPTY;
            }
            if (prev) {
                prev->next = curr->next;
            } else {
                dir_node->children = curr->next;
            }
            tmpfs_node_t* target = curr->node;
            kfree(curr);
            tmpfs_node_unref(target);
            kmutex_unlock(&fs->lock);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    kmutex_unlock(&fs->lock);
    return -ENOENT;
}

static int tmpfs_op_rename(vnode_t* old_dir, const char* old_name, vnode_t* new_dir, const char* new_name) {
    if (!old_dir || !old_dir->fs_data || !new_dir || !new_dir->fs_data || !old_name || !new_name) return -EINVAL;
    tmpfs_node_t* old_dnode = (tmpfs_node_t*) old_dir->fs_data;
    tmpfs_node_t* new_dnode = (tmpfs_node_t*) new_dir->fs_data;
    tmpfs_fs_t* fs = old_dnode->fs;

    kmutex_lock(&fs->lock);

    /* Locate old entry */
    tmpfs_dirent_t* prev_old = NULL;
    tmpfs_dirent_t* curr_old = old_dnode->children;
    while (curr_old && strcmp(curr_old->name, old_name) != 0) {
        prev_old = curr_old;
        curr_old = curr_old->next;
    }
    if (!curr_old) {
        kmutex_unlock(&fs->lock);
        return -ENOENT;
    }

    /* Check if destination exists in new_dir and replace it */
    tmpfs_dirent_t* prev_new = NULL;
    tmpfs_dirent_t* curr_new = new_dnode->children;
    while (curr_new && strcmp(curr_new->name, new_name) != 0) {
        prev_new = curr_new;
        curr_new = curr_new->next;
    }

    if (curr_new) {
        if (curr_new->node->type == TMPFS_NODE_DIR && curr_new->node->children != NULL) {
            kmutex_unlock(&fs->lock);
            return -ENOTEMPTY;
        }
        if (prev_new) {
            prev_new->next = curr_new->next;
        } else {
            new_dnode->children = curr_new->next;
        }
        tmpfs_node_unref(curr_new->node);
        kfree(curr_new);
    }

    /* Remove from old_dir */
    if (prev_old) {
        prev_old->next = curr_old->next;
    } else {
        old_dnode->children = curr_old->next;
    }

    /* Update name and insert into new_dir */
    strncpy(curr_old->name, new_name, NAME_MAX);
    curr_old->name[NAME_MAX] = '\0';
    curr_old->next = new_dnode->children;
    new_dnode->children = curr_old;

    kmutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_op_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    if (!dir || !dir->fs_data || !entry) return -EINVAL;
    tmpfs_node_t* dir_node = (tmpfs_node_t*) dir->fs_data;
    tmpfs_fs_t* fs = dir_node->fs;

    kmutex_lock(&fs->lock);

    if (index == 0) {
        entry->d_ino = dir_node->ino;
        entry->d_type = 2; /* directory */
        entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        kmutex_unlock(&fs->lock);
        return 1;
    }
    if (index == 1) {
        entry->d_ino = dir->parent ? ((tmpfs_node_t*)dir->parent->fs_data)->ino : dir_node->ino;
        entry->d_type = 2; /* directory */
        entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        kmutex_unlock(&fs->lock);
        return 1;
    }

    uint32_t target_idx = index - 2;
    uint32_t cur_idx = 0;
    tmpfs_dirent_t* curr = dir_node->children;
    while (curr) {
        if (cur_idx == target_idx) {
            entry->d_ino = curr->node->ino;
            entry->d_type = (curr->node->type == TMPFS_NODE_DIR) ? 2 : 1;
            entry->d_size = curr->node->size;
            strncpy(entry->d_name, curr->name, NAME_MAX);
            entry->d_name[NAME_MAX] = '\0';
            kmutex_unlock(&fs->lock);
            return 1;
        }
        cur_idx++;
        curr = curr->next;
    }

    kmutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_op_truncate(vnode_t* node, uint32_t length) {
    if (!node || !node->fs_data) return -EINVAL;
    tmpfs_node_t* tnode = (tmpfs_node_t*) node->fs_data;
    tmpfs_fs_t* fs = tnode->fs;

    kmutex_lock(&fs->lock);

    if (length < tnode->size) {
        /* Free pages beyond length */
        uint32_t first_dead_page = (length + FRAME_SIZE - 1) / FRAME_SIZE;
        uint32_t total_pages = (tnode->size + FRAME_SIZE - 1) / FRAME_SIZE;

        for (uint32_t p = first_dead_page; p < total_pages; p++) {
            uint32_t b = p / TMPFS_PAGES_PER_BLOCK;
            uint32_t o = p % TMPFS_PAGES_PER_BLOCK;
            if (b < TMPFS_MAX_BLOCKS && tnode->page_blocks[b]) {
                uint32_t phys = tnode->page_blocks[b][o];
                if (phys != 0) {
                    pmm_unref(phys);
                    if (fs->used_frames > 0) fs->used_frames--;
                    tnode->page_blocks[b][o] = 0;
                }
            }
        }

        /* Zero remainder of boundary page if truncated mid-page */
        uint32_t boundary_page = length / FRAME_SIZE;
        uint32_t boundary_off = length % FRAME_SIZE;
        if (boundary_off != 0) {
            uint32_t phys = tmpfs_get_or_alloc_frame(tnode, boundary_page, 0, NULL);
            if (phys != 0) {
                memset((uint8_t*)P2V(phys) + boundary_off, 0, FRAME_SIZE - boundary_off);
            }
        }
    }

    tnode->size = length;
    node->size = length;

    kmutex_unlock(&fs->lock);
    return 0;
}

static int tmpfs_op_stat(vnode_t* node, struct stat* st) {
    if (!node || !node->fs_data || !st) return -EINVAL;
    tmpfs_node_t* tnode = (tmpfs_node_t*) node->fs_data;

    st->st_dev = 0;
    st->st_ino = tnode->ino;
    st->st_mode = tnode->mode;
    st->st_nlink = tnode->refcount;
    st->st_size = tnode->size;
    st->st_blksize = FRAME_SIZE;
    st->st_blocks = (tnode->size + 511) / 512;
    st->st_atime = tnode->atime;
    st->st_mtime = tnode->mtime;
    st->st_ctime = tnode->ctime;

    return 0;
}

static int tmpfs_op_cleanup(vnode_t* node) {
    if (!node || !node->fs_data) return 0;
    tmpfs_node_t* tnode = (tmpfs_node_t*) node->fs_data;
    tmpfs_node_unref(tnode);
    return 0;
}

const struct fs_ops tmpfs_ops = {
    .lookup   = tmpfs_op_lookup,
    .read     = tmpfs_op_read,
    .write    = tmpfs_op_write,
    .create   = tmpfs_op_create,
    .mkdir    = tmpfs_op_mkdir,
    .unlink   = tmpfs_op_unlink,
    .rmdir    = tmpfs_op_rmdir,
    .rename   = tmpfs_op_rename,
    .readdir  = tmpfs_op_readdir,
    .truncate = tmpfs_op_truncate,
    .stat     = tmpfs_op_stat,
    .cleanup  = tmpfs_op_cleanup
};

int tmpfs_mount(const char* mount_path) {
    tmpfs_fs_t* fs = (tmpfs_fs_t*) kmalloc(sizeof(tmpfs_fs_t));
    if (!fs) return -ENOMEM;
    memset(fs, 0, sizeof(tmpfs_fs_t));

    kmutex_init(&fs->lock, "tmpfs");
    fs->used_frames = 0;
    /* Size cap: default 25% of total RAM frames */
    fs->max_frames = pmm_total_count() / 4;
    if (fs->max_frames == 0) fs->max_frames = 1024;

    tmpfs_node_t* root = tmpfs_alloc_node(fs, TMPFS_NODE_DIR, S_IFDIR | 0777);
    if (!root) {
        kfree(fs);
        return -ENOMEM;
    }
    fs->root = root;

    vnode_t* vn = vnode_alloc(VNODE_DIR, &tmpfs_ops, root);
    if (!vn) {
        tmpfs_node_unref(root);
        kfree(fs);
        return -ENOMEM;
    }
    tmpfs_node_ref(root);

    int ret = vfs_mount(mount_path, vn);
    if (ret != 0) {
        vnode_unref(vn);
        return ret;
    }

    kprintf("[tmpfs] Mounted tmpfs on %s (cap: %u MB / %u frames)\n",
            mount_path, (fs->max_frames * 4) / 1024, fs->max_frames);
    return 0;
}

void tmpfs_init(void) {
    /* Ready for mounting */
}

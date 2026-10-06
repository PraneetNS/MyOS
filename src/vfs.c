#include "vfs.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "kheap.h"
#include "pipe.h"

#define MAX_MOUNTS 4
#define VNODE_POOL_SIZE 128
#define OPEN_FILE_POOL_SIZE 64

static int active_open_files = 0;

int vfs_get_active_open_files(void) {
    return active_open_files;
}

typedef struct {
    char path[64];
    vnode_t* root;
    int in_use;
} mount_point_t;

static mount_point_t mount_table[MAX_MOUNTS];
static vnode_t vnode_pool[VNODE_POOL_SIZE];
static open_file_t of_pool[OPEN_FILE_POOL_SIZE];

static int console_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    return keyboard_read_line((char*) buf, (int) count);
}

static int console_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    for (uint32_t i = 0; i < count; i++) {
        terminal_putchar(buf[i]);
        serial_putc(buf[i]);
    }
    return (int) count;
}

static int console_stat(vnode_t* vn, struct stat* st) {
    (void) vn;
    st->st_dev = 0;
    st->st_ino = 1;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static const struct fs_ops console_ops = {
    .lookup = 0,
    .read = console_read,
    .write = console_write,
    .create = 0,
    .mkdir = 0,
    .unlink = 0,
    .rmdir = 0,
    .rename = 0,
    .readdir = 0,
    .truncate = 0,
    .stat = console_stat,
};

static vnode_t console_vnode = {
    .type = VNODE_CONSOLE,
    .size = 0,
    .fs_data = 0,
    .ops = &console_ops,
    .refcount = 1000,
    .parent = 0,
};

void vfs_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_table[i].in_use = 0;
        mount_table[i].path[0] = '\0';
        mount_table[i].root = 0;
    }
    for (int i = 0; i < VNODE_POOL_SIZE; i++) {
        vnode_pool[i].refcount = 0;
    }
    for (int i = 0; i < OPEN_FILE_POOL_SIZE; i++) {
        of_pool[i].refcount = 0;
        of_pool[i].type = OPEN_FILE_NONE;
    }
}

int vfs_mount(const char* path, vnode_t* root_vnode) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].in_use) {
            int j = 0;
            while (path[j] && j < 63) {
                mount_table[i].path[j] = path[j];
                j++;
            }
            mount_table[i].path[j] = '\0';
            mount_table[i].root = root_vnode;
            mount_table[i].in_use = 1;
            vnode_ref(root_vnode);
            return 0;
        }
    }
    return -ENOMEM;
}

vnode_t* vfs_get_root(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use && mount_table[i].path[0] == '/' && mount_table[i].path[1] == '\0') {
            return mount_table[i].root;
        }
    }
    return 0;
}

vnode_t* vnode_alloc(vnode_type_t type, const struct fs_ops* ops, void* fs_data) {
    for (int i = 0; i < VNODE_POOL_SIZE; i++) {
        if (vnode_pool[i].refcount == 0) {
            vnode_pool[i].type = type;
            vnode_pool[i].ops = ops;
            vnode_pool[i].fs_data = fs_data;
            vnode_pool[i].size = 0;
            vnode_pool[i].refcount = 1;
            vnode_pool[i].parent = 0;
            return &vnode_pool[i];
        }
    }
    return 0;
}

void vnode_ref(vnode_t* vn) {
    if (vn) vn->refcount++;
}

void vnode_unref(vnode_t* vn) {
    if (vn && vn->refcount > 0) {
        vn->refcount--;
        if (vn->refcount == 0 && vn->parent) {
            vnode_unref(vn->parent);
            vn->parent = 0;
        }
    }
}

open_file_t* open_file_alloc(open_file_type_t type, vnode_t* vn, int flags) {
    for (int i = 0; i < OPEN_FILE_POOL_SIZE; i++) {
        if (of_pool[i].refcount == 0) {
            of_pool[i].type = type;
            of_pool[i].vnode = vn;
            of_pool[i].pipe = 0;
            of_pool[i].flags = flags;
            of_pool[i].offset = 0;
            of_pool[i].refcount = 1;
            if (vn) vnode_ref(vn);
            active_open_files++;
            return &of_pool[i];
        }
    }
    return 0;
}

void open_file_ref(open_file_t* of) {
    if (!of) return;
    if (of->refcount <= 0) {
        kprintf("[vfs] ASSERTION FAILED: open_file %p refcount <= 0 in ref (%d)\n", of, of->refcount);
    }
    of->refcount++;
}

void open_file_unref(open_file_t* of) {
    if (!of) return;
    if (of->refcount <= 0) {
        kprintf("[vfs] ASSERTION FAILED: open_file %p refcount <= 0 in unref (%d)\n", of, of->refcount);
        return;
    }
    of->refcount--;
    if (of->refcount == 0) {
        if (of->type == OPEN_FILE_VNODE && of->vnode) {
            vnode_unref(of->vnode);
            of->vnode = 0;
        } else if (of->type == OPEN_FILE_PIPE && of->pipe) {
            pipe_close_end((pipe_t*) of->pipe, of->flags);
            of->pipe = 0;
        }
        of->type = OPEN_FILE_NONE;
        active_open_files--;
        if (active_open_files < 0) {
            kprintf("[vfs] ASSERTION FAILED: active_open_files negative: %d\n", active_open_files);
        }
    }
}

open_file_t* vfs_get_console_stdin(void) {
    return open_file_alloc(OPEN_FILE_CONSOLE, &console_vnode, O_RDONLY);
}

open_file_t* vfs_get_console_stdout(void) {
    return open_file_alloc(OPEN_FILE_CONSOLE, &console_vnode, O_WRONLY);
}

open_file_t* vfs_get_console_stderr(void) {
    return open_file_alloc(OPEN_FILE_CONSOLE, &console_vnode, O_WRONLY);
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int vfs_resolve_path(const char* path, const char* cwd, vnode_t** out) {
    if (!path || !*path || !out) return -EINVAL;

    vnode_t* curr = 0;
    const char* p = path;

    if (*p == '/') {
        curr = vfs_get_root();
        while (*p == '/') p++;
    } else {
        if (cwd && *cwd) {
            if (vfs_resolve_path(cwd, "/", &curr) != 0) {
                curr = vfs_get_root();
            }
        } else {
            curr = vfs_get_root();
        }
    }

    if (!curr) return -ENOENT;

    char token[64];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        int idx = 0;
        while (*p && *p != '/' && idx < 63) {
            token[idx++] = *p++;
        }
        token[idx] = '\0';

        if (streq(token, ".")) {
            continue;
        }
        if (streq(token, "..")) {
            if (curr->parent) {
                curr = curr->parent;
            }
            continue;
        }

        if (curr->type != VNODE_DIR) {
            return -ENOTDIR;
        }
        if (!curr->ops || !curr->ops->lookup) {
            return -ENOSYS;
        }

        vnode_t* next = 0;
        int ret = curr->ops->lookup(curr, token, &next);
        if (ret != 0 || !next) {
            return -ENOENT;
        }

        if (!next->parent) {
            next->parent = curr;
            vnode_ref(curr);
        }
        curr = next;
    }

    *out = curr;
    return 0;
}

int vfs_resolve_parent(const char* path, const char* cwd, vnode_t** dir_out, char* name_out, int name_max) {
    if (!path || !*path || !dir_out || !name_out) return -EINVAL;

    /* Find the last slash */
    int len = 0;
    int last_slash = -1;
    while (path[len]) {
        if (path[len] == '/') last_slash = len;
        len++;
    }

    /* Trailing slash removal if any */
    while (last_slash == len - 1 && last_slash > 0) {
        len--;
        last_slash = -1;
        for (int i = 0; i < len; i++) {
            if (path[i] == '/') last_slash = i;
        }
    }

    if (last_slash < 0) {
        /* No slash: parent is cwd */
        int ret = vfs_resolve_path(cwd ? cwd : "/", "/", dir_out);
        if (ret != 0) return ret;
        int j = 0;
        while (path[j] && j < name_max - 1) {
            name_out[j] = path[j];
            j++;
        }
        name_out[j] = '\0';
        return 0;
    }

    /* Extract parent path */
    char parent_path[128];
    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        int i = 0;
        while (i < last_slash && i < 127) {
            parent_path[i] = path[i];
            i++;
        }
        parent_path[i] = '\0';
    }

    int ret = vfs_resolve_path(parent_path, cwd, dir_out);
    if (ret != 0) return ret;

    /* Extract child name */
    const char* child_start = path + last_slash + 1;
    int j = 0;
    while (child_start[j] && j < name_max - 1) {
        name_out[j] = child_start[j];
        j++;
    }
    name_out[j] = '\0';

    return 0;
}

int vfs_read(open_file_t* of, void* buf, uint32_t count) {
    if (!of) return -EBADF;

    if (of->type == OPEN_FILE_CONSOLE) {
        return console_read(of->vnode, 0, (uint8_t*) buf, count);
    }
    if (of->type == OPEN_FILE_PIPE) {
        if ((of->flags & 3) == O_WRONLY) return -EBADF;
        return pipe_read((pipe_t*) of->pipe, buf, count);
    }
    if (of->type == OPEN_FILE_VNODE && of->vnode) {
        if (of->vnode->type == VNODE_DIR) return -EISDIR;
        if (!of->vnode->ops || !of->vnode->ops->read) return -EINVAL;
        int n = of->vnode->ops->read(of->vnode, of->offset, (uint8_t*) buf, count);
        if (n > 0) of->offset += (uint32_t) n;
        return n;
    }
    return -EBADF;
}

int vfs_write(open_file_t* of, const void* buf, uint32_t count) {
    if (!of) return -EBADF;

    if (of->type == OPEN_FILE_CONSOLE) {
        return console_write(of->vnode, 0, (const uint8_t*) buf, count);
    }
    if (of->type == OPEN_FILE_PIPE) {
        if ((of->flags & 3) == O_RDONLY) return -EBADF;
        return pipe_write((pipe_t*) of->pipe, buf, count);
    }
    if (of->type == OPEN_FILE_VNODE && of->vnode) {
        if (of->vnode->type == VNODE_DIR) return -EISDIR;
        if (!of->vnode->ops || !of->vnode->ops->write) return -EROFS;
        if (of->flags & O_APPEND) of->offset = of->vnode->size;
        int n = of->vnode->ops->write(of->vnode, of->offset, (const uint8_t*) buf, count);
        if (n > 0) of->offset += (uint32_t) n;
        return n;
    }
    return -EBADF;
}

int vfs_close(open_file_t* of) {
    if (!of) return -EBADF;
    open_file_unref(of);
    return 0;
}

int vfs_lseek(open_file_t* of, int offset, int whence) {
    if (!of) return -EBADF;
    if (of->type == OPEN_FILE_CONSOLE || of->type == OPEN_FILE_PIPE) return -ESPIPE;
    if (of->type != OPEN_FILE_VNODE || !of->vnode) return -EBADF;

    int new_offset;
    if (whence == SEEK_SET) {
        new_offset = offset;
    } else if (whence == SEEK_CUR) {
        new_offset = (int)of->offset + offset;
    } else if (whence == SEEK_END) {
        new_offset = (int)of->vnode->size + offset;
    } else {
        return -EINVAL;
    }

    if (new_offset < 0) return -EINVAL;
    of->offset = (uint32_t) new_offset;
    return new_offset;
}

int vfs_readdir(open_file_t* of, struct dirent* entry) {
    if (!of) return -EBADF;
    if (of->type != OPEN_FILE_VNODE || !of->vnode) return -EBADF;
    if (of->vnode->type != VNODE_DIR) return -ENOTDIR;
    if (!of->vnode->ops || !of->vnode->ops->readdir) return -ENOSYS;

    int ret = of->vnode->ops->readdir(of->vnode, of->offset, entry);
    if (ret == 1) {
        of->offset++;
        return 1;
    }
    return ret;
}

int vfs_stat(vnode_t* vn, struct stat* st) {
    if (!vn || !st) return -EINVAL;
    if (vn->ops && vn->ops->stat) {
        return vn->ops->stat(vn, st);
    }
    st->st_dev = 0;
    st->st_ino = 1;
    st->st_mode = (vn->type == VNODE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = vn->size;
    st->st_blksize = 512;
    st->st_blocks = (vn->size + 511) / 512;
    return 0;
}

void vfs_path_canonical(const char* path, const char* cwd, char* out, int max) {
    if (!out || max <= 1) return;
    char combined[128];
    int p = 0;
    if (path && path[0] == '/') {
        while (path[p] && p < 126) {
            combined[p] = path[p];
            p++;
        }
        combined[p] = '\0';
    } else {
        const char* c = (cwd && *cwd) ? cwd : "/";
        while (c[p] && p < 120) {
            combined[p] = c[p];
            p++;
        }
        if (p > 0 && combined[p - 1] != '/') {
            combined[p++] = '/';
        }
        int j = 0;
        while (path && path[j] && p < 126) {
            combined[p++] = path[j++];
        }
        combined[p] = '\0';
    }

    char segs[16][32];
    int nsegs = 0;
    int i = 0;
    while (combined[i]) {
        while (combined[i] == '/') i++;
        if (!combined[i]) break;
        char tok[32];
        int tlen = 0;
        while (combined[i] && combined[i] != '/' && tlen < 31) {
            tok[tlen++] = combined[i++];
        }
        tok[tlen] = '\0';
        while (combined[i] && combined[i] != '/') i++;

        if (tok[0] == '.' && tok[1] == '\0') {
            continue;
        } else if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0') {
            if (nsegs > 0) nsegs--;
        } else {
            if (nsegs < 16) {
                int k = 0;
                while (tok[k]) { segs[nsegs][k] = tok[k]; k++; }
                segs[nsegs][k] = '\0';
                nsegs++;
            }
        }
    }

    if (nsegs == 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    int pos = 0;
    for (int s = 0; s < nsegs; s++) {
        if (pos < max - 1) out[pos++] = '/';
        int k = 0;
        while (segs[s][k] && pos < max - 1) {
            out[pos++] = segs[s][k++];
        }
    }
    out[pos] = '\0';
}

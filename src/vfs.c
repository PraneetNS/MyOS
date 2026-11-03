#include "vfs.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "kheap.h"
#include "pipe.h"
#include "socket.h"
#include "scheduler.h"
#include "io.h"

#define MAX_MOUNTS 16
#define VNODE_POOL_SIZE 256
#define OPEN_FILE_POOL_SIZE 256

static int active_open_files = 0;

int vfs_get_active_open_files(void) {
    return active_open_files;
}

int vfs_get_current_flags(void) {
    process_t* me = scheduler_current();
    if (me) return me->current_op_flags;
    return 0;
}

typedef struct {
    char path[64];
    char fstype[16];
    char dev[32];
    vnode_t* root;
    int in_use;
} mount_point_t;

static mount_point_t mount_table[MAX_MOUNTS];
static vnode_t vnode_pool[VNODE_POOL_SIZE];
static open_file_t of_pool[OPEN_FILE_POOL_SIZE];

void vfs_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_table[i].in_use = 0;
        mount_table[i].path[0] = '\0';
        mount_table[i].fstype[0] = '\0';
        mount_table[i].dev[0] = '\0';
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

int vfs_mount_type(const char* dev, const char* path, const char* fstype, vnode_t* root_vnode) {
    if (!path || !root_vnode) return -EINVAL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mount_table[i].in_use) {
            int j = 0;
            while (path[j] && j < 63) {
                mount_table[i].path[j] = path[j];
                j++;
            }
            mount_table[i].path[j] = '\0';

            const char* d = dev ? dev : "none";
            j = 0;
            while (d[j] && j < 31) {
                mount_table[i].dev[j] = d[j];
                j++;
            }
            mount_table[i].dev[j] = '\0';

            const char* f = fstype ? fstype : "vfs";
            j = 0;
            while (f[j] && j < 15) {
                mount_table[i].fstype[j] = f[j];
                j++;
            }
            mount_table[i].fstype[j] = '\0';

            mount_table[i].root = root_vnode;
            mount_table[i].in_use = 1;
            vnode_ref(root_vnode);
            return 0;
        }
    }
    return -ENOMEM;
}

int vfs_mount(const char* path, vnode_t* root_vnode) {
    return vfs_mount_type("none", path, "vfs", root_vnode);
}

int vfs_get_mount_info(int idx, char* dev_out, char* path_out, char* fstype_out) {
    if (idx < 0 || idx >= MAX_MOUNTS) return 0;
    if (!mount_table[idx].in_use) return 0;

    if (dev_out) {
        int i = 0;
        while (mount_table[idx].dev[i]) { dev_out[i] = mount_table[idx].dev[i]; i++; }
        dev_out[i] = '\0';
    }
    if (path_out) {
        int i = 0;
        while (mount_table[idx].path[i]) { path_out[i] = mount_table[idx].path[i]; i++; }
        path_out[i] = '\0';
    }
    if (fstype_out) {
        int i = 0;
        while (mount_table[idx].fstype[i]) { fstype_out[i] = mount_table[idx].fstype[i]; i++; }
        fstype_out[i] = '\0';
    }
    return 1;
}

vnode_t* vfs_get_root(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use && mount_table[i].path[0] == '/' && mount_table[i].path[1] == '\0') {
            return mount_table[i].root;
        }
    }
    return 0;
}

int vfs_truncate(vnode_t* vn, uint32_t length) {
    if (!vn) return -EINVAL;
    if (!vn->ops || !vn->ops->truncate) return -EINVAL;
    return vn->ops->truncate(vn, length);
}

static inline uint64_t vfs_lock(void) {
    return irq_save();
}

static inline void vfs_unlock(uint64_t flags) {
    irq_restore(flags);
}

vnode_t* vnode_alloc(vnode_type_t type, const struct fs_ops* ops, void* fs_data) {
    uint64_t f = vfs_lock();
    for (int i = 0; i < VNODE_POOL_SIZE; i++) {
        if (vnode_pool[i].refcount == 0) {
            vnode_pool[i].type = type;
            vnode_pool[i].ops = ops;
            vnode_pool[i].fs_data = fs_data;
            vnode_pool[i].size = 0;
            vnode_pool[i].refcount = 1;
            vnode_pool[i].parent = 0;
            vfs_unlock(f);
            return &vnode_pool[i];
        }
    }
    vfs_unlock(f);
    return 0;
}

void vnode_ref(vnode_t* vn) {
    if (!vn) return;
    uint64_t f = vfs_lock();
    vn->refcount++;
    vfs_unlock(f);
}

void vnode_unref(vnode_t* vn) {
    if (!vn) return;
    uint64_t f = vfs_lock();
    if (vn->refcount > 0) {
        if (vn->refcount > 500) { vfs_unlock(f); return; } /* permanent vnode (e.g. devfs) */
        vn->refcount--;
        if (vn->refcount == 0) {
            const struct fs_ops* ops = vn->ops;
            void* fs_data = vn->fs_data;
            vn->parent = 0;
            vn->fs_data = 0;
            vfs_unlock(f);

            if (ops && ops->cleanup) {
                vnode_t tmp = *vn;
                tmp.fs_data = fs_data;
                ops->cleanup(&tmp);
            }
            return;
        }
    }
    vfs_unlock(f);
}

void vfs_for_each_vnode(void (*cb)(vnode_t*, void*), void* arg) {
    if (!cb) return;
    uint64_t f = vfs_lock();
    for (int i = 0; i < VNODE_POOL_SIZE; i++) {
        if (vnode_pool[i].refcount > 0) {
            cb(&vnode_pool[i], arg);
        }
    }
    vfs_unlock(f);
}

open_file_t* open_file_alloc(open_file_type_t type, vnode_t* vn, int flags) {
    uint64_t f = vfs_lock();
    for (int i = 0; i < OPEN_FILE_POOL_SIZE; i++) {
        if (of_pool[i].refcount == 0) {
            of_pool[i].type = type;
            of_pool[i].vnode = vn;
            of_pool[i].pipe = 0;
            of_pool[i].sock = 0;
            of_pool[i].flags = flags;
            of_pool[i].offset = 0;
            of_pool[i].refcount = 1;
            of_pool[i].path[0] = '\0';
            if (vn) vnode_ref(vn);
            active_open_files++;
            vfs_unlock(f);
            return &of_pool[i];
        }
    }
    vfs_unlock(f);
    return 0;
}

void open_file_ref(open_file_t* of) {
    if (!of) return;
    uint64_t f = vfs_lock();
    if (of->refcount <= 0) {
        kprintf("[vfs] ASSERTION FAILED: open_file %p refcount <= 0 in ref (%d)\n", of, of->refcount);
    }
    of->refcount++;
    vfs_unlock(f);
}

void open_file_unref(open_file_t* of) {
    if (!of) return;
    uint64_t f = vfs_lock();
    if (of->refcount <= 0) {
        kprintf("[vfs] ASSERTION FAILED: open_file %p refcount <= 0 in unref (%d)\n", of, of->refcount);
        vfs_unlock(f);
        return;
    }
    of->refcount--;
    if (of->refcount == 0) {
        if (of->type == OPEN_FILE_CONSOLE) {
            process_t* cur = scheduler_current();
            if (cur && console_get_graphics_pid() != 0 && console_get_graphics_pid() == cur->pid) {
                console_restore_text_mode();
            }
        }
        vnode_t* vto_unref = 0;
        pipe_t* pto_close = 0;
        socket_t* sto_close = 0;
        int pflags = of->flags;
        if ((of->type == OPEN_FILE_VNODE || of->type == OPEN_FILE_CONSOLE) && of->vnode) {
            vto_unref = of->vnode;
            of->vnode = 0;
        } else if (of->type == OPEN_FILE_PIPE && of->pipe) {
            pto_close = (pipe_t*) of->pipe;
            of->pipe = 0;
        } else if (of->type == OPEN_FILE_SOCKET && of->sock) {
            sto_close = (socket_t*) of->sock;
            of->sock = 0;
        }
        of->type = OPEN_FILE_NONE;
        active_open_files--;
        if (active_open_files < 0) {
            kprintf("[vfs] ASSERTION FAILED: active_open_files negative: %d\n", active_open_files);
        }
        vfs_unlock(f);
        if (vto_unref) vnode_unref(vto_unref);
        if (pto_close) pipe_close_end(pto_close, pflags);
        if (sto_close) {
            socket_close(sto_close);
            socket_unref(sto_close);
        }
        return;
    }
    vfs_unlock(f);
}

open_file_t* vfs_get_console_stdin(void) {
    vnode_t* vn = 0;
    if (vfs_resolve_path("/dev/console", "/", &vn) == 0 && vn) {
        return open_file_alloc(OPEN_FILE_CONSOLE, vn, O_RDONLY);
    }
    return 0;
}

open_file_t* vfs_get_console_stdout(void) {
    vnode_t* vn = 0;
    if (vfs_resolve_path("/dev/console", "/", &vn) == 0 && vn) {
        return open_file_alloc(OPEN_FILE_CONSOLE, vn, O_WRONLY);
    }
    return 0;
}

open_file_t* vfs_get_console_stderr(void) {
    vnode_t* vn = 0;
    if (vfs_resolve_path("/dev/console", "/", &vn) == 0 && vn) {
        return open_file_alloc(OPEN_FILE_CONSOLE, vn, O_WRONLY);
    }
    return 0;
}

static int kstrlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int vfs_is_symlink(vnode_t* vn, char* target_out, int max_len) {
    if (!vn || vn->type != VNODE_FILE || !vn->ops || !vn->ops->read) return 0;

    int is_proc_lnk = 0;
    if (vn->size == 0) {
        if (!vn->ops->stat) return 0;
        struct stat st;
        if (vn->ops->stat(vn, &st) != 0 || !S_ISLNK(st.st_mode)) return 0;
        is_proc_lnk = 1;
    } else if (vn->size < 9 || vn->size > 1040) {
        if (!vn->ops->stat) return 0;
        struct stat st;
        if (vn->ops->stat(vn, &st) != 0 || !S_ISLNK(st.st_mode)) return 0;
        is_proc_lnk = 1;
    }

    uint8_t buf[520];
    int n = vn->ops->read(vn, 0, buf, 512);
    if (n <= 0) return 0;

    int start_off = 0;
    if (n >= 9 &&
        buf[0] == 'M' && buf[1] == 'Y' && buf[2] == 'O' && buf[3] == 'S' &&
        buf[4] == 'L' && buf[5] == 'N' && buf[6] == 'K' && buf[7] == '\n') {
        start_off = 8;
    } else if (!is_proc_lnk) {
        return 0;
    }

    int len = 0;
    while (start_off + len < n && buf[start_off + len] != '\0' &&
           buf[start_off + len] != '\n' && buf[start_off + len] != '\r') {
        if (target_out && len < max_len - 1) {
            target_out[len] = (char)buf[start_off + len];
        }
        len++;
    }
    if (target_out && max_len > 0) {
        int term = (len < max_len - 1) ? len : (max_len - 1);
        target_out[term] = '\0';
    }
    return len;
}

int vfs_resolve_path_ex(const char* path, const char* cwd, vnode_t** out, int follow_last, char* canon_out, int canon_max) {
    if (!path || !*path || !out) return -EINVAL;
    if (kstrlen(path) >= PATH_MAX) return -ENAMETOOLONG;

    char canon[PATH_MAX];
    vfs_path_canonical(path, cwd, canon, sizeof(canon));

    int symlink_depth = 0;

restart_walk:
    if (canon_out && canon_max > 0) {
        int ci = 0;
        while (canon[ci] && ci < canon_max - 1) { canon_out[ci] = canon[ci]; ci++; }
        canon_out[ci] = '\0';
    }

    int best_mount = -1;
    int best_len = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mount_table[i].in_use) {
            int mlen = 0;
            while (mount_table[i].path[mlen]) mlen++;
            int match = 1;
            for (int k = 0; k < mlen; k++) {
                if (canon[k] != mount_table[i].path[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if (mlen == 1 && mount_table[i].path[0] == '/') {
                    if (mlen > best_len) {
                        best_len = mlen;
                        best_mount = i;
                    }
                } else if (canon[mlen] == '\0' || canon[mlen] == '/') {
                    if (mlen > best_len) {
                        best_len = mlen;
                        best_mount = i;
                    }
                }
            }
        }
    }

    if (best_mount < 0) return -ENOENT;

    vnode_t* curr = mount_table[best_mount].root;
    if (!curr) return -ENOENT;

    const char* p = canon + best_len;
    while (*p == '/') p++;
    if (!*p) {
        *out = curr;
        vnode_ref(curr);
        return 0;
    }

    char token[NAME_MAX + 1];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char* token_start_in_canon = p;
        int idx = 0;
        while (*p && *p != '/') {
            if (idx >= NAME_MAX) {
                if (curr != mount_table[best_mount].root) vnode_unref(curr);
                return -ENAMETOOLONG;
            }
            token[idx++] = *p++;
        }
        token[idx] = '\0';

        const char* after_token = p;
        while (*after_token == '/') after_token++;
        int is_last = (*after_token == '\0');

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
            if (curr != mount_table[best_mount].root) vnode_unref(curr);
            return -ENOTDIR;
        }
        if (!curr->ops || !curr->ops->lookup) {
            if (curr != mount_table[best_mount].root) vnode_unref(curr);
            return -ENOSYS;
        }

        vnode_t* next = 0;
        int ret = curr->ops->lookup(curr, token, &next);
        if (ret != 0 || !next) {
            if (curr != mount_table[best_mount].root) {
                vnode_unref(curr);
            }
            return -ENOENT;
        }

        if (!next->parent) {
            next->parent = curr;
        }
        if (curr != mount_table[best_mount].root) {
            vnode_unref(curr);
        }

        /* Check if `next` is a symlink that should be followed */
        if (!is_last || follow_last) {
            char link_target[512];
            if (next->type == VNODE_FILE && vfs_is_symlink(next, link_target, sizeof(link_target)) > 0) {
                if (next != mount_table[best_mount].root) {
                    vnode_unref(next);
                }
                symlink_depth++;
                if (symlink_depth > 8) {
                    return -ELOOP;
                }

                /* Build new path: if link_target is relative, base is canon[0 .. token_start_in_canon - 1] */
                char dir_base[PATH_MAX];
                if (link_target[0] == '/') {
                    dir_base[0] = '/';
                    dir_base[1] = '\0';
                } else {
                    int dlen = (int)(token_start_in_canon - canon);
                    while (dlen > 1 && canon[dlen - 1] == '/') dlen--;
                    if (dlen <= 0) {
                        dir_base[0] = '/';
                        dir_base[1] = '\0';
                    } else {
                        for (int k = 0; k < dlen && k < PATH_MAX - 1; k++) dir_base[k] = canon[k];
                        dir_base[dlen < PATH_MAX - 1 ? dlen : PATH_MAX - 1] = '\0';
                    }
                }

                char combined_target[PATH_MAX];
                int cp = 0;
                for (int k = 0; link_target[k] && cp < PATH_MAX - 1; k++) {
                    combined_target[cp++] = link_target[k];
                }
                if (*after_token) {
                    if (cp > 0 && combined_target[cp - 1] != '/' && cp < PATH_MAX - 1) {
                        combined_target[cp++] = '/';
                    }
                    for (int k = 0; after_token[k] && cp < PATH_MAX - 1; k++) {
                        combined_target[cp++] = after_token[k];
                    }
                }
                combined_target[cp] = '\0';

                vfs_path_canonical(combined_target, dir_base, canon, sizeof(canon));
                goto restart_walk;
            }
        }

        curr = next;
    }

    *out = curr;
    return 0;
}

int vfs_resolve_path(const char* path, const char* cwd, vnode_t** out) {
    return vfs_resolve_path_ex(path, cwd, out, 1, 0, 0);
}

int vfs_resolve_parent_ex(const char* path, const char* cwd, vnode_t** dir_out, char* name_out, int name_max, char* parent_canon_out, int parent_canon_max) {
    if (!path || !*path || !dir_out || !name_out) return -EINVAL;

    /* Find the last slash */
    int len = 0;
    int last_slash = -1;
    while (path[len]) {
        if (path[len] == '/') last_slash = len;
        len++;
    }
    if (len >= PATH_MAX) return -ENAMETOOLONG;

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
        if (len > NAME_MAX) return -ENAMETOOLONG;
        int ret = vfs_resolve_path_ex(cwd ? cwd : "/", "/", dir_out, 1, parent_canon_out, parent_canon_max);
        if (ret != 0) return ret;
        int j = 0;
        while (j < len && j < name_max - 1) {
            name_out[j] = path[j];
            j++;
        }
        name_out[j] = '\0';
        return 0;
    }

    /* Extract parent path */
    char parent_path[PATH_MAX];
    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        int i = 0;
        while (i < last_slash && i < (int)sizeof(parent_path) - 1) {
            parent_path[i] = path[i];
            i++;
        }
        parent_path[i] = '\0';
    }

    int ret = vfs_resolve_path_ex(parent_path, cwd, dir_out, 1, parent_canon_out, parent_canon_max);
    if (ret != 0) return ret;

    const char* child_start = path + last_slash + 1;
    int child_len = len - (last_slash + 1);
    if (child_len > NAME_MAX) {
        vnode_unref(*dir_out);
        *dir_out = 0;
        return -ENAMETOOLONG;
    }

    int k = 0;
    while (k < child_len && k < name_max - 1) {
        name_out[k] = child_start[k];
        k++;
    }
    name_out[k] = '\0';
    return 0;
}

int vfs_resolve_parent(const char* path, const char* cwd, vnode_t** dir_out, char* name_out, int name_max) {
    return vfs_resolve_parent_ex(path, cwd, dir_out, name_out, name_max, 0, 0);
}

int vfs_read(open_file_t* of, void* buf, uint32_t count) {
    if (!of) return -EBADF;

    int nonblock = (of->flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    process_t* me = scheduler_current();
    if (me) me->current_op_flags = of->flags;

    if (of->type == OPEN_FILE_CONSOLE) {
        if ((of->flags & 3) == O_WRONLY) return -EBADF;
        if (!of->vnode || !of->vnode->ops || !of->vnode->ops->read) return -EBADF;
        return of->vnode->ops->read(of->vnode, of->offset, (uint8_t*) buf, count);
    }
    if (of->type == OPEN_FILE_PIPE) {
        if ((of->flags & 3) == O_WRONLY) return -EBADF;
        return pipe_read((pipe_t*) of->pipe, buf, count, nonblock);
    }
    if (of->type == OPEN_FILE_SOCKET) {
        if ((of->flags & 3) == O_WRONLY) return -EBADF;
        return socket_recv((socket_t*) of->sock, buf, count, 0);
    }
    if (of->type == OPEN_FILE_VNODE && of->vnode) {
        if ((of->flags & 3) == O_WRONLY) return -EBADF;
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

    int nonblock = (of->flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    process_t* me = scheduler_current();
    if (me) me->current_op_flags = of->flags;

    if (of->type == OPEN_FILE_CONSOLE) {
        if ((of->flags & 3) == O_RDONLY) return -EBADF;
        if (!of->vnode || !of->vnode->ops || !of->vnode->ops->write) return -EBADF;
        return of->vnode->ops->write(of->vnode, of->offset, (const uint8_t*) buf, count);
    }
    if (of->type == OPEN_FILE_PIPE) {
        if ((of->flags & 3) == O_RDONLY) return -EBADF;
        return pipe_write((pipe_t*) of->pipe, buf, count, nonblock);
    }
    if (of->type == OPEN_FILE_SOCKET) {
        if ((of->flags & 3) == O_RDONLY) return -EBADF;
        return socket_send((socket_t*) of->sock, buf, count, 0);
    }
    if (of->type == OPEN_FILE_VNODE && of->vnode) {
        if ((of->flags & 3) == O_RDONLY) return -EBADF;
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
    if (of->type == OPEN_FILE_CONSOLE || of->type == OPEN_FILE_PIPE || of->type == OPEN_FILE_SOCKET) return -ESPIPE;
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
    int r = 0;
    if (vn->ops && vn->ops->stat) {
        r = vn->ops->stat(vn, st);
        if (r != 0) return r;
    } else {
        st->st_dev = 0;
        st->st_ino = 1;
        st->st_mode = (vn->type == VNODE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        st->st_nlink = 1;
        st->st_size = vn->size;
        st->st_blksize = 512;
        st->st_blocks = (vn->size + 511) / 512;
    }
    if (vn->type == VNODE_FILE) {
        int slen = vfs_is_symlink(vn, 0, 0);
        if (slen > 0) {
            st->st_mode = S_IFLNK | 0777;
            st->st_size = (uint32_t)slen;
        }
    }
    return 0;
}

void vfs_path_canonical(const char* path, const char* cwd, char* out, int max) {
    if (!out || max <= 1) return;
    char combined[PATH_MAX + 128];
    int p = 0;
    if (path && path[0] == '/') {
        while (path[p] && p < PATH_MAX) {
            combined[p] = path[p];
            p++;
        }
        combined[p] = '\0';
    } else {
        const char* c = (cwd && *cwd) ? cwd : "/";
        int cp = 0;
        while (c[cp] && p < PATH_MAX) {
            combined[p++] = c[cp++];
        }
        if (p > 0 && combined[p - 1] != '/' && p < PATH_MAX) {
            combined[p++] = '/';
        }
        int j = 0;
        while (path && path[j] && p < PATH_MAX) {
            combined[p++] = path[j++];
        }
        combined[p] = '\0';
    }

    int seg_start[64];
    int seg_len[64];
    int nsegs = 0;
    int i = 0;
    while (combined[i]) {
        while (combined[i] == '/') i++;
        if (!combined[i]) break;
        int start = i;
        while (combined[i] && combined[i] != '/') i++;
        int len = i - start;

        if (len == 1 && combined[start] == '.') {
            continue;
        } else if (len == 2 && combined[start] == '.' && combined[start + 1] == '.') {
            if (nsegs > 0) nsegs--;
        } else {
            if (nsegs < 64) {
                seg_start[nsegs] = start;
                seg_len[nsegs] = len;
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
        for (int k = 0; k < seg_len[s] && pos < max - 1; k++) {
            out[pos++] = combined[seg_start[s] + k];
        }
    }
    out[pos] = '\0';
}

short vfs_poll(open_file_t* of, short events, void** chan_out) {
    if (!of) return POLLNVAL;

    if (of->type == OPEN_FILE_PIPE) {
        pipe_t* p = (pipe_t*) of->pipe;
        if (!p) return POLLNVAL;
        short revents = 0;
        int is_write = ((of->flags & 3) == O_WRONLY);
        int is_read = ((of->flags & 3) == O_RDONLY || (of->flags & 3) == O_RDWR);

        if (is_read) {
            if (p->count > 0) revents |= POLLIN;
            if (p->writer_count == 0) revents |= POLLHUP;
            if (!(revents & (POLLIN | POLLHUP)) && (events & POLLIN) && chan_out && !*chan_out) {
                *chan_out = &p->read_wait;
            }
        }
        if (is_write) {
            if (p->reader_count == 0) {
                revents |= POLLERR;
            } else if (p->count < PIPE_CAPACITY) {
                revents |= POLLOUT;
            } else if ((events & POLLOUT) && chan_out && !*chan_out) {
                *chan_out = &p->write_wait;
            }
        }
        return revents;
    }

    if (of->type == OPEN_FILE_SOCKET) {
        return socket_poll((socket_t*) of->sock, events, chan_out);
    }

    if (of->type == OPEN_FILE_CONSOLE) {
        if (of->vnode && of->vnode->ops && of->vnode->ops->poll) {
            return of->vnode->ops->poll(of->vnode, events, chan_out);
        }
        return (POLLIN | POLLOUT) & events;
    }

    if (of->type == OPEN_FILE_VNODE && of->vnode) {
        if (of->vnode->ops && of->vnode->ops->poll) {
            return of->vnode->ops->poll(of->vnode, events, chan_out);
        }
        return (POLLIN | POLLOUT) & events;
    }

    return POLLNVAL;
}

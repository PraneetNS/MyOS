#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "process.h"
#include "fs.h"
#include "kheap.h"
#include "pipe.h"
#include "elf.h"
#include "usermode.h"
#include "vmm.h"
#include "serial.h"
#include "keyboard.h"
#include "bcache.h"
#include "vfs.h"
#include "errno.h"
#include "timer.h"
#include "pmm.h"
#include "paging.h"
#include "vma.h"
#include "uaccess.h"
#include "tty.h"
#include "devfs.h"
#include "signal.h"
#include "rtc.h"
#include "kstack.h"
#include "fpu.h"
#include "kmutex.h"
#include "tmpfs.h"
#include "socket.h"
#include "pty.h"
#include "random.h"
#include "agent.h"

#include "../libmyos/include/myos_syscalls.h"

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANON        0x20
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void*)-1)

#define WNOHANG        1

extern void isr128(void); /* defined in isr.s */

struct syscall_args {
    uint64_t eax;  /* Syscall number on entry, return value on exit */
    uint64_t ebx;  /* Arg 1 */
    uint64_t ecx;  /* Arg 2 */
    uint64_t edx;  /* Arg 3 */
    uint64_t esi;  /* Arg 4 */
    uint64_t edi;  /* Arg 5 */
    uint64_t ebp;  /* Arg 6 */
    uint64_t cs;
    uint64_t rflags;
    struct registers* raw;
};

static int validate_user_buffer(const void* ptr, uint64_t len, int write) {
    if (len == 0) return 0;
    if (!ptr) return -EFAULT;

    process_t* me = scheduler_current();
    if (!me || me->is_kernel_task) return 0;

    uint64_t start = (uint64_t) ptr;
    uint64_t end = start + len;
    if (end < start) return -EFAULT;
    if (start < 0x1000 || end > 0x0000800000000000ULL) return -EFAULT;

    if (me->vma_list) {
        /* Auto-expand stack VMA downward if buffer is within valid user stack range */
        if (start >= USER_STACK_BOTTOM && end <= USER_STACK_TOP) {
            vma_t* sv = me->vma_list;
            while (sv) {
                if (sv->flags & VMA_FLAG_STACK) {
                    if (start < sv->start) {
                        sv->start = start & ~0xFFFULL;
                    }
                    break;
                }
                sv = sv->next;
            }
        }

        uint64_t cur = start;
        while (cur < end) {
            vma_t* vma = vma_find(me, cur);
            if (!vma) return -EFAULT;
            if (write && !(vma->prot & VMA_PROT_WRITE)) return -EFAULT;
            if (!write && !(vma->prot & VMA_PROT_READ)) return -EFAULT;
            cur = vma->end;
        }
        return 0;
    }

    if (!me->as.pml4) return 0;
    uint64_t* pml4 = me->as.pml4;
    uint64_t page_start = start & ~0xFFFULL;
    uint64_t page_end = (end - 1) & ~0xFFFULL;

    for (uint64_t va = page_start;; va += 4096) {
        uint64_t pml4_idx = (va >> 39) & 0x1FF;
        uint64_t pdpt_idx = (va >> 30) & 0x1FF;
        uint64_t pd_idx   = (va >> 21) & 0x1FF;
        uint64_t pt_idx   = (va >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PAGE_PRESENT) || !(pml4[pml4_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pdpt = (uint64_t*) P2V(pml4[pml4_idx] & ~0xFFFULL);
        if (!(pdpt[pdpt_idx] & PAGE_PRESENT) || !(pdpt[pdpt_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);
        if (!(pd[pd_idx] & PAGE_PRESENT) || !(pd[pd_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);
        if (!(pt[pt_idx] & PAGE_PRESENT) || !(pt[pt_idx] & PAGE_USER)) return -EFAULT;
        if (write && !(pt[pt_idx] & PAGE_WRITE)) return -EFAULT;

        if (va >= page_end) break;
    }

    return 0;
}

static int validate_user_string(const char* s) {
    if (!s) return -EFAULT;

    process_t* me = scheduler_current();
    if (!me || me->is_kernel_task) return 0;

    uint64_t va = (uint64_t) s;
    if (va < 0x1000 || va >= 0x0000800000000000ULL) return -EFAULT;

    if (me->vma_list) {
        if (va >= USER_STACK_BOTTOM && va < USER_STACK_TOP) {
            vma_t* sv = me->vma_list;
            while (sv) {
                if (sv->flags & VMA_FLAG_STACK) {
                    if (va < sv->start) {
                        sv->start = va & ~0xFFFULL;
                    }
                    break;
                }
                sv = sv->next;
            }
        }
        vma_t* vma = vma_find(me, va);
        if (!vma || !(vma->prot & VMA_PROT_READ)) return -EFAULT;
        int len = 0;
        while (1) {
            if (va >= vma->end) {
                vma = vma_find(me, va);
                if (!vma || !(vma->prot & VMA_PROT_READ)) return -EFAULT;
            }
            char c = *(const char*) va;
            if (c == '\0') return 0;
            len++;
            if (len > 1024) return -ENAMETOOLONG;
            va++;
            if (va >= 0x0000800000000000ULL) return -EFAULT;
        }
    }

    if (!me->as.pml4) return 0;
    uint64_t* pml4 = me->as.pml4;
    int len = 0;

    while (1) {
        if (va < 0x1000 || va >= 0x0000800000000000ULL) return -EFAULT;
        uint64_t pml4_idx = (va >> 39) & 0x1FF;
        uint64_t pdpt_idx = (va >> 30) & 0x1FF;
        uint64_t pd_idx   = (va >> 21) & 0x1FF;
        uint64_t pt_idx   = (va >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PAGE_PRESENT) || !(pml4[pml4_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pdpt = (uint64_t*) P2V(pml4[pml4_idx] & ~0xFFFULL);
        if (!(pdpt[pdpt_idx] & PAGE_PRESENT) || !(pdpt[pdpt_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);
        if (!(pd[pd_idx] & PAGE_PRESENT) || !(pd[pd_idx] & PAGE_USER)) return -EFAULT;
        uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);
        if (!(pt[pt_idx] & PAGE_PRESENT) || !(pt[pt_idx] & PAGE_USER)) return -EFAULT;

        uint64_t page_limit = (va & ~0xFFFULL) + 4096;
        while (va < page_limit) {
            char c = *(const char*) va;
            if (c == '\0') return 0;
            len++;
            if (len > 1024) return -ENAMETOOLONG;
            va++;
            if (va >= 0x0000800000000000ULL) return -EFAULT;
        }
    }
}



typedef struct {
    uint32_t mask;
    uint32_t num_fs_read;
    agent_path_rule_t fs_read[MAX_PATH_RULES];
    uint32_t num_fs_write;
    agent_path_rule_t fs_write[MAX_PATH_RULES];
    uint32_t num_exec;
    agent_path_rule_t exec[MAX_PATH_RULES];
    uint32_t num_connect;
    agent_path_rule_t connect[MAX_PATH_RULES];
    uint32_t num_bus_call;
    agent_path_rule_t bus_call[MAX_PATH_RULES];
    uint32_t num_bus_provide;
    agent_path_rule_t bus_provide[MAX_PATH_RULES];
} agent_caps_v3_t;

typedef struct {
    uint32_t mask;
    uint32_t num_fs_read;
    agent_path_rule_t fs_read[MAX_PATH_RULES];
    uint32_t num_fs_write;
    agent_path_rule_t fs_write[MAX_PATH_RULES];
    uint32_t num_exec;
    agent_path_rule_t exec[MAX_PATH_RULES];
    uint32_t num_connect;
    agent_path_rule_t connect[MAX_PATH_RULES];
    uint32_t num_bus_call;
    agent_path_rule_t bus_call[MAX_PATH_RULES];
    uint32_t num_bus_provide;
    agent_path_rule_t bus_provide[MAX_PATH_RULES];
    uint32_t num_window_observe;
    agent_path_rule_t window_observe[MAX_PATH_RULES];
    uint32_t num_window_act;
    agent_path_rule_t window_act[MAX_PATH_RULES];
    uint32_t model_backends;
} agent_caps_v4_t;

typedef struct {
    int id;
    char name[AGENT_NAME_MAX];
    int owner_pid;
    int parent_agent_id;
    agent_state_t state;
    agent_caps_v3_t caps;
    agent_budget_t budget;
    agent_usage_t usage;
} agent_info_v3_t;

typedef struct {
    int id;
    char name[AGENT_NAME_MAX];
    int owner_pid;
    int parent_agent_id;
    agent_state_t state;
    agent_caps_v4_t caps;
    agent_budget_t budget;
    agent_usage_t usage;
} agent_info_v4_t;

static size_t kstrlen(const char* s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static const char* kstrstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return 0;
}

static void kernel_stat_to_linux_stat64(const struct stat* ks, struct linux_stat64* ls) {
    memset(ls, 0, sizeof(*ls));
    ls->st_dev = ks->st_dev;
    ls->st_ino = ks->st_ino;
    ls->st_mode = ks->st_mode;
    ls->st_nlink = ks->st_nlink;
    ls->st_uid = 0;
    ls->st_gid = 0;
    ls->st_rdev = 0;
    ls->st_size = ks->st_size;
    ls->st_blksize = ks->st_blksize ? ks->st_blksize : 512;
    ls->st_blocks = ks->st_blocks;
    ls->st_atim.tv_sec = ks->st_atime;
    ls->st_atim.tv_nsec = 0;
    ls->st_mtim.tv_sec = ks->st_mtime;
    ls->st_mtim.tv_nsec = 0;
    ls->st_ctim.tv_sec = ks->st_ctime;
    ls->st_ctim.tv_nsec = 0;
}

static int resolve_dirfd_base(process_t* me, int dirfd, const char* path, const char** base_out) {
    if (path && path[0] == '/') {
        *base_out = "/";
        return 0;
    }
    if (dirfd == AT_FDCWD) {
        *base_out = me->cwd;
        return 0;
    }
    if (dirfd < 0 || dirfd >= MAX_FDS || !me->fds[dirfd]) {
        return -EBADF;
    }
    open_file_t* dof = me->fds[dirfd];
    if (dof->type != OPEN_FILE_VNODE || !dof->vnode || dof->vnode->type != VNODE_DIR) {
        return -ENOTDIR;
    }
    *base_out = dof->path[0] ? dof->path : me->cwd;
    return 0;
}

static int do_sys_openat(int dirfd, const char* path, int flags, int mode) {
    (void) mode;
    int err = validate_user_string(path);
    if (err != 0) return err;

    process_t* me = scheduler_current();
    const char* base_cwd = me->cwd;
    err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
    if (err != 0) return err;

    char full_path[PATH_MAX];
    vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));

    if (me->agent_id > 0) {
        if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
            int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, "open_write");
            if (aerr < 0) return aerr;
        }
        if (((flags & 3) == O_RDONLY) || (flags & O_RDWR)) {
            int aerr = agent_check_access(me, CAP_FS_READ, full_path, "open_read");
            if (aerr < 0) return aerr;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!me->fds[i]) { slot = i; break; }
    }
    if (slot < 0) return -EMFILE;

    vnode_t* vn = 0;
    char resolved_canon[PATH_MAX];
    resolved_canon[0] = '\0';
    int follow = (flags & O_NOFOLLOW) ? 0 : 1;
    int ret = vfs_resolve_path_ex(path, base_cwd, &vn, follow, resolved_canon, sizeof(resolved_canon));
    if (ret == 0 && vn) {
        if ((flags & O_NOFOLLOW) && vfs_is_symlink(vn, 0, 0) > 0) {
            vnode_unref(vn);
            return -ELOOP;
        }
        if ((flags & O_DIRECTORY) && vn->type != VNODE_DIR) {
            vnode_unref(vn);
            return -ENOTDIR;
        }
        if (me->agent_id > 0 && resolved_canon[0]) {
            if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, resolved_canon, "open_write");
                if (aerr < 0) { vnode_unref(vn); return aerr; }
            }
            if (((flags & 3) == O_RDONLY) || (flags & O_RDWR)) {
                int aerr = agent_check_access(me, CAP_FS_READ, resolved_canon, "open_read");
                if (aerr < 0) { vnode_unref(vn); return aerr; }
            }
        }
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            vnode_unref(vn);
            return -EEXIST;
        }
        if (vn->type == VNODE_DIR && ((flags & O_WRONLY) || (flags & O_RDWR))) {
            vnode_unref(vn);
            return -EISDIR;
        }
        if (flags & O_TRUNC) {
            if (vn->ops && vn->ops->truncate) {
                vn->ops->truncate(vn, 0);
                vn->size = 0;
            }
        }
    } else {
        if (ret == -ELOOP || ret == -ENOTDIR || ret == -ENAMETOOLONG) {
            return ret;
        }
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        /* Create file */
        vnode_t* parent = 0;
        char child_name[NAME_MAX + 1];
        char parent_canon[PATH_MAX];
        parent_canon[0] = '\0';
        ret = vfs_resolve_parent_ex(path, base_cwd, &parent, child_name, sizeof(child_name), parent_canon, sizeof(parent_canon));
        if (ret != 0 || !parent) {
            return (ret ? ret : -ENOENT);
        }
        if (me->agent_id > 0 && parent_canon[0]) {
            char target_canon[PATH_MAX];
            vfs_path_canonical(child_name, parent_canon, target_canon, sizeof(target_canon));
            int aerr = agent_check_access(me, CAP_FS_WRITE, target_canon, "open_create");
            if (aerr < 0) { vnode_unref(parent); return aerr; }
            int ci = 0;
            while (target_canon[ci] && ci < PATH_MAX - 1) { resolved_canon[ci] = target_canon[ci]; ci++; }
            resolved_canon[ci] = '\0';
        } else {
            vfs_path_canonical(child_name, parent_canon[0] ? parent_canon : base_cwd, resolved_canon, sizeof(resolved_canon));
        }
        if (!parent->ops || !parent->ops->create) {
            vnode_unref(parent);
            return -ENOSYS;
        }
        ret = parent->ops->create(parent, child_name, &vn);
        vnode_unref(parent);
        if (ret != 0 || !vn) {
            return ret;
        }
    }

    if (devfs_is_ptmx_vnode(vn)) {
        vnode_t* master_vn = NULL;
        int pty_err = pty_create(&master_vn);
        vnode_unref(vn);
        if (pty_err != 0 || !master_vn) {
            return (pty_err ? pty_err : -ENOMEM);
        }
        vn = master_vn;
    }

    open_file_type_t of_type = (vn->type == VNODE_CONSOLE) ? OPEN_FILE_CONSOLE : OPEN_FILE_VNODE;
    open_file_t* of = open_file_alloc(of_type, vn, flags);
    vnode_unref(vn);
    if (!of) {
        return -ENFILE;
    }
    if (flags & O_APPEND) {
        of->offset = vn->size;
    }
    const char* psrc = resolved_canon[0] ? resolved_canon : full_path;
    int pi = 0;
    while (psrc[pi] && pi < 255) { of->path[pi] = psrc[pi]; pi++; }
    of->path[pi] = '\0';

    me->fds[slot] = of;
    me->fd_flags[slot] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return slot;
}

static int do_sys_fstat(int fd, struct linux_stat64* u_ls) {
    int err = validate_user_buffer(u_ls, sizeof(struct linux_stat64), 1);
    if (err != 0) return err;

    process_t* me = scheduler_current();
    if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
        return -EBADF;
    }

    struct stat ks;
    memset(&ks, 0, sizeof(ks));
    int ret = -EBADF;

    open_file_t* of = me->fds[fd];
    if (of->type == OPEN_FILE_CONSOLE) {
        if (of->vnode) {
            ret = vfs_stat(of->vnode, &ks);
        } else {
            ks.st_dev = 0; ks.st_ino = 0; ks.st_mode = S_IFCHR | 0666;
            ks.st_nlink = 1; ks.st_size = 0; ks.st_blksize = 512; ks.st_blocks = 0;
            ret = 0;
        }
    } else if (of->type == OPEN_FILE_PIPE) {
        ks.st_mode = S_IFIFO | 0600;
        ks.st_nlink = 1;
        ks.st_blksize = 512;
        ret = 0;
    } else if (of->type == OPEN_FILE_VNODE && of->vnode) {
        ret = vfs_stat(of->vnode, &ks);
    } else if (of->type == OPEN_FILE_SOCKET && of->sock) {
        ret = socket_stat((socket_t*) of->sock, &ks);
    }

    if (ret == 0) {
        struct linux_stat64 ls;
        kernel_stat_to_linux_stat64(&ks, &ls);
        if (copy_to_user(u_ls, &ls, sizeof(ls)) != 0) {
            return -EFAULT;
        }
    }
    return ret;
}

static int do_sys_statat(int dirfd, const char* path, struct linux_stat64* u_ls, int flags) {
    int err = validate_user_string(path);
    if (err != 0) return err;

    if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
        return do_sys_fstat(dirfd, u_ls);
    }

    err = validate_user_buffer(u_ls, sizeof(struct linux_stat64), 1);
    if (err != 0) return err;

    process_t* me = scheduler_current();
    const char* base_cwd = me->cwd;
    err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
    if (err != 0) return err;

    char full_path[PATH_MAX];
    vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
    if (me->agent_id > 0) {
        int aerr = agent_check_access(me, CAP_FS_READ, full_path, "stat");
        if (aerr < 0) return aerr;
    }

    vnode_t* vn = 0;
    char resolved_canon[PATH_MAX];
    resolved_canon[0] = '\0';
    int follow = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : 1;
    int ret = vfs_resolve_path_ex(path, base_cwd, &vn, follow, resolved_canon, sizeof(resolved_canon));
    if (ret != 0 || !vn) {
        return (ret ? ret : -ENOENT);
    }

    if (me->agent_id > 0 && resolved_canon[0]) {
        int aerr = agent_check_access(me, CAP_FS_READ, resolved_canon, "stat");
        if (aerr < 0) {
            vnode_unref(vn);
            return aerr;
        }
    }

    struct stat ks;
    memset(&ks, 0, sizeof(ks));
    ret = vfs_stat(vn, &ks);
    vnode_unref(vn);
    if (ret == 0) {
        struct linux_stat64 ls;
        kernel_stat_to_linux_stat64(&ks, &ls);
        if (copy_to_user(u_ls, &ls, sizeof(ls)) != 0) {
            return -EFAULT;
        }
    }
    return ret;
}

static int do_sys_symlinkat(const char* target, int newdirfd, const char* linkpath) {
    int err = validate_user_string(target);
    if (err != 0) return err;
    err = validate_user_string(linkpath);
    if (err != 0) return err;

    process_t* me = scheduler_current();
    const char* base_cwd = me->cwd;
    err = resolve_dirfd_base(me, newdirfd, linkpath, &base_cwd);
    if (err != 0) return err;

    char full_path[PATH_MAX];
    vfs_path_canonical(linkpath, base_cwd, full_path, sizeof(full_path));
    if (me->agent_id > 0) {
        int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, "symlink");
        if (aerr < 0) return aerr;
    }

    vnode_t* existing = 0;
    if (vfs_resolve_path_ex(linkpath, base_cwd, &existing, 0, 0, 0) == 0 && existing) {
        vnode_unref(existing);
        return -EEXIST;
    }

    vnode_t* parent = 0;
    char child_name[NAME_MAX + 1];
    char parent_canon[PATH_MAX];
    parent_canon[0] = '\0';
    int ret = vfs_resolve_parent_ex(linkpath, base_cwd, &parent, child_name, sizeof(child_name), parent_canon, sizeof(parent_canon));
    if (ret != 0 || !parent) return (ret ? ret : -ENOENT);

    if (me->agent_id > 0 && parent_canon[0]) {
        char target_canon[PATH_MAX];
        vfs_path_canonical(child_name, parent_canon, target_canon, sizeof(target_canon));
        int aerr = agent_check_access(me, CAP_FS_WRITE, target_canon, "symlink");
        if (aerr < 0) { vnode_unref(parent); return aerr; }
    }

    if (!parent->ops || !parent->ops->create) {
        vnode_unref(parent);
        return -ENOSYS;
    }

    vnode_t* vn = 0;
    ret = parent->ops->create(parent, child_name, &vn);
    vnode_unref(parent);
    if (ret != 0 || !vn) return (ret ? ret : -EIO);

    char* sym_buf = (char*) kmalloc(520);
    if (!sym_buf) {
        vnode_unref(vn);
        return -ENOMEM;
    }
    const char* hdr = "MYOSLNK\n";
    int pos = 0;
    for (int i = 0; i < 8; i++) sym_buf[pos++] = hdr[i];
    for (int i = 0; target[i] && pos < 511; i++) sym_buf[pos++] = target[i];

    if (!vn->ops || !vn->ops->write) {
        kfree(sym_buf);
        vnode_unref(vn);
        return -EIO;
    }
    int wn = vn->ops->write(vn, 0, (const uint8_t*)sym_buf, (uint32_t)pos);
    kfree(sym_buf);
    vnode_unref(vn);
    return (wn == pos) ? 0 : -EIO;
}

static int do_sys_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    if (bufsiz == 0) return -EINVAL;
    int err = validate_user_string(path);
    if (err != 0) return err;
    err = validate_user_buffer(buf, bufsiz, 1);
    if (err != 0) return err;

    process_t* me = scheduler_current();
    const char* base_cwd = me->cwd;
    err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
    if (err != 0) return err;

    char full_path[PATH_MAX];
    vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
    if (me->agent_id > 0) {
        int aerr = agent_check_access(me, CAP_FS_READ, full_path, "readlink");
        if (aerr < 0) return aerr;
    }

    vnode_t* vn = 0;
    char resolved_canon[PATH_MAX];
    resolved_canon[0] = '\0';
    int ret = vfs_resolve_path_ex(path, base_cwd, &vn, 0, resolved_canon, sizeof(resolved_canon));
    if (ret != 0 || !vn) return (ret ? ret : -ENOENT);

    if (me->agent_id > 0 && resolved_canon[0]) {
        int aerr = agent_check_access(me, CAP_FS_READ, resolved_canon, "readlink");
        if (aerr < 0) {
            vnode_unref(vn);
            return aerr;
        }
    }

    char target_buf[512];
    int len = vfs_is_symlink(vn, target_buf, sizeof(target_buf));
    vnode_unref(vn);
    if (len <= 0) return -EINVAL;

    size_t to_copy = ((size_t)len < bufsiz) ? (size_t)len : bufsiz;
    if (copy_to_user(buf, target_buf, (uint32_t)to_copy) != 0) return -EFAULT;
    return (int)to_copy;
}

static void syscall_handler_internal(struct syscall_args* regs) {
    agent_killswitch_check_deferred();
    process_t* me_syscall_proc = scheduler_current();
    if (me_syscall_proc) me_syscall_proc->current_syscall = regs->eax;
    switch (regs->eax) {
        case SYS_WRITE: {
            int fd = (int) regs->ebx;
            const void* buf = (const void*) regs->ecx;
            uint32_t len = regs->edx;

            int err = validate_user_buffer(buf, len, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            int n = vfs_write(me->fds[fd], buf, len);
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_EXIT:
        case SYS_EXIT_GROUP: {
            int status = (int) regs->ebx;
            kprintf("[ok] process exited (pid %d, status %d)\n", scheduler_current()->pid, status);
            scheduler_exit_current(status); /* never returns */
            break;
        }

        case SYS_GETPID:
        case __NR_gettid:
            regs->eax = (uint32_t) scheduler_current()->pid;
            break;

        case SYS_OPEN: {
            const char* path = (const char*) regs->ebx;
            int flags = (int) regs->ecx;
            int mode = (int) regs->edx;
            regs->eax = (uint32_t) do_sys_openat(AT_FDCWD, path, flags, mode);
            break;
        }

        case SYS_OPENAT: {
            int dirfd = (int) regs->ebx;
            const char* path = (const char*) regs->ecx;
            int flags = (int) regs->edx;
            int mode = (int) regs->esi;
            regs->eax = (uint32_t) do_sys_openat(dirfd, path, flags, mode);
            break;
        }

        case __NR_creat: {
            const char* path = (const char*) regs->ebx;
            int mode = (int) regs->ecx;
            regs->eax = (uint32_t) do_sys_openat(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
            break;
        }

        case SYS_READ: {
            int fd = (int) regs->ebx;
            void* buf = (void*) regs->ecx;
            uint32_t len = regs->edx;

            int err = validate_user_buffer(buf, len, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            int n = vfs_read(me->fds[fd], buf, len);
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_CLOSE: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd >= 0 && fd < MAX_FDS && me->fds[fd]) {
                open_file_t* of = me->fds[fd];
                me->fds[fd] = 0;
                me->fd_flags[fd] = 0;
                vfs_close(of);
                regs->eax = 0;
            } else {
                regs->eax = (uint32_t) -EBADF;
            }
            break;
        }



        case SYS_FORK:
        case __NR_vfork: {
            process_t* me = scheduler_current();
            process_t* child = process_fork(me, regs->raw);
            regs->eax = child ? (uint64_t) child->pid : (uint64_t) -EAGAIN;
            break;
        }

        case SYS_CLONE: {
            unsigned long clone_flags = (unsigned long) regs->ebx;
            void* child_stack = (void*) regs->ecx;
            int* ptid = (int*) regs->edx;
            int* ctid = (int*) regs->esi;
            unsigned long newtls = (unsigned long) regs->edi;

            /* CLONE_VM (0x100) without CLONE_VFORK (0x4000) is thread creation (Phase 9B) */
            if ((clone_flags & 0x00000100UL) && !(clone_flags & 0x00004000UL)) {
                regs->eax = (uint64_t) -ENOSYS;
                break;
            }

            process_t* me = scheduler_current();
            process_t* child = process_fork(me, regs->raw);
            if (!child) {
                regs->eax = (uint64_t) -EAGAIN;
                break;
            }
            if (child_stack) {
                child->saved_regs.rsp = (uint64_t) child_stack;
            }
            if (clone_flags & 0x00080000UL /* CLONE_SETTLS */) {
                child->fs_base = newtls;
            }
            if ((clone_flags & 0x00200000UL /* CLONE_CHILD_CLEARTID */) && ctid) {
                child->clear_child_tid = ctid;
            }
            if ((clone_flags & 0x00010000UL /* CLONE_PARENT_SETTID */) && ptid) {
                if (validate_user_buffer(ptid, sizeof(int), 1) == 0) {
                    *ptid = child->pid;
                }
            }
            regs->eax = (uint64_t) child->pid;
            break;
        }

        case SYS_PIPE:
        case SYS_PIPE2: {
            int* user_fds = (int*) regs->ebx;
            int pipe_flags = (regs->eax == SYS_PIPE2) ? (int) regs->ecx : 0;
            int err = validate_user_buffer(user_fds, sizeof(int) * 2, 1);
            if (err != 0) {
                regs->eax = (uint32_t) err;
                break;
            }

            process_t* me = scheduler_current();
            int fd0 = -1, fd1 = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!me->fds[i]) {
                    if (fd0 < 0) fd0 = i;
                    else if (fd1 < 0) { fd1 = i; break; }
                }
            }

            if (fd0 < 0 || fd1 < 0) {
                regs->eax = (uint32_t) -EMFILE;
                break;
            }

            pipe_t* p = 0;
            err = pipe_create(&p);
            if (err != 0) {
                regs->eax = (uint32_t) err;
                break;
            }

            int rflags = O_RDONLY | (pipe_flags & (O_NONBLOCK | O_NONBLOCK_LINUX));
            int wflags = O_WRONLY | (pipe_flags & (O_NONBLOCK | O_NONBLOCK_LINUX));
            open_file_t* of_read = open_file_alloc(OPEN_FILE_PIPE, 0, rflags);
            if (!of_read) {
                pipe_close_end(p, O_RDONLY);
                pipe_close_end(p, O_WRONLY);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }

            open_file_t* of_write = open_file_alloc(OPEN_FILE_PIPE, 0, wflags);
            if (!of_write) {
                of_read->pipe = 0;
                open_file_unref(of_read);
                pipe_close_end(p, O_RDONLY);
                pipe_close_end(p, O_WRONLY);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }

            of_read->pipe = p;
            of_write->pipe = p;

            uint8_t cloexec = (pipe_flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            me->fds[fd0] = of_read;
            me->fd_flags[fd0] = cloexec;
            me->fds[fd1] = of_write;
            me->fd_flags[fd1] = cloexec;

            user_fds[0] = fd0;
            user_fds[1] = fd1;
            regs->eax = 0;
            break;
        }

        case SYS_EXEC: {
            const char* name = (const char*) regs->ebx;
            const char* const* uargv = (const char* const*) regs->ecx;
            const char* const* uenvp = (const char* const*) regs->edx;

            int err = validate_user_string(name);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_EXEC, name, "exec");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }

            typedef struct {
                char kname[PATH_MAX];
                char bin_path[PATH_MAX];
                char resolved_canon[PATH_MAX];
                char kargv_buf[32][NAME_MAX + 1];
                char kenvp_buf[32][128];
            } exec_params_t;

            exec_params_t* ep = (exec_params_t*) kmalloc(sizeof(exec_params_t));
            if (!ep) {
                regs->eax = (uint32_t) -ENOMEM;
                break;
            }

            int ki = 0;
            while (name[ki] && ki < PATH_MAX - 1) {
                ep->kname[ki] = name[ki];
                ki++;
            }
            ep->kname[ki] = '\0';
            ep->resolved_canon[0] = '\0';

            vnode_t* vn = 0;
            if (vfs_resolve_path_ex(ep->kname, me->cwd, &vn, 1, ep->resolved_canon, sizeof(ep->resolved_canon)) != 0 || !vn) {
                ep->bin_path[0] = '/'; ep->bin_path[1] = 'b'; ep->bin_path[2] = 'i'; ep->bin_path[3] = 'n'; ep->bin_path[4] = '/';
                int l = 0;
                while (ep->kname[l] && l < PATH_MAX - 6) {
                    ep->bin_path[5 + l] = ep->kname[l];
                    l++;
                }
                ep->bin_path[5 + l] = '\0';
                if (vfs_resolve_path_ex(ep->bin_path, me->cwd, &vn, 1, ep->resolved_canon, sizeof(ep->resolved_canon)) != 0 || !vn) {
                    kfree(ep);
                    regs->eax = (uint32_t) -ENOENT;
                    break;
                }
            }

            if (me->agent_id > 0 && ep->resolved_canon[0]) {
                int aerr = agent_check_access(me, CAP_EXEC, ep->resolved_canon, "exec");
                if (aerr < 0) {
                    vnode_unref(vn);
                    kfree(ep);
                    regs->eax = (uint32_t) aerr;
                    break;
                }
            }

            /* Copy arguments from user space before tearing down address space */
            int argc = 0;
            const char* kargv[33];
            if (uargv) {
                int bad = 0;
                while (argc < 32) {
                    if (validate_user_buffer(&uargv[argc], sizeof(char*), 0) != 0) {
                        bad = 1; break;
                    }
                    const char* uarg = uargv[argc];
                    if (!uarg) break;
                    if (validate_user_string(uarg) != 0) {
                        bad = 1; break;
                    }
                    int j = 0;
                    while (uarg[j] && j < NAME_MAX) {
                        ep->kargv_buf[argc][j] = uarg[j];
                        j++;
                    }
                    ep->kargv_buf[argc][j] = '\0';
                    kargv[argc] = ep->kargv_buf[argc];
                    argc++;
                }
                if (bad) {
                    vnode_unref(vn);
                    kfree(ep);
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }
            if (argc == 0) {
                int j = 0;
                while (ep->kname[j] && j < NAME_MAX) {
                    ep->kargv_buf[0][j] = ep->kname[j];
                    j++;
                }
                ep->kargv_buf[0][j] = '\0';
                kargv[0] = ep->kargv_buf[0];
                argc = 1;
            }
            kargv[argc] = 0;

            /* Copy environment variables from user space if provided, or default */
            int envc = 0;
            const char* kenvp[33];
            static const char* const default_env[5] = {
                "HOME=/",
                "PWD=/",
                "USER=root",
                "PATH=/bin:/usr/bin:/",
                0
            };

            if (uenvp) {
                int bad = 0;
                while (envc < 32) {
                    if (validate_user_buffer(&uenvp[envc], sizeof(char*), 0) != 0) {
                        bad = 1; break;
                    }
                    const char* uenv = uenvp[envc];
                    if (!uenv) break;
                    if (validate_user_string(uenv) != 0) {
                        bad = 1; break;
                    }
                    int j = 0;
                    while (uenv[j] && j < 127) {
                        ep->kenvp_buf[envc][j] = uenv[j];
                        j++;
                    }
                    ep->kenvp_buf[envc][j] = '\0';
                    kenvp[envc] = ep->kenvp_buf[envc];
                    envc++;
                }
                if (bad) {
                    vnode_unref(vn);
                    kfree(ep);
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
                kenvp[envc] = 0;
            } else {
                for (int i = 0; default_env[i]; i++) {
                    kenvp[i] = default_env[i];
                    envc++;
                }
                kenvp[envc] = 0;
            }

            if (vn->size == 0) {
                vnode_unref(vn);
                kfree(ep);
                regs->eax = (uint32_t) -ENOEXEC;
                break;
            }

            uint32_t alloc_size = ((vn->size + 511) / 512) * 512;
            uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
            if (!buf) {
                vnode_unref(vn);
                kfree(ep);
                regs->eax = (uint32_t) -ENOMEM;
                break;
            }

            int n = vn->ops->read(vn, 0, buf, vn->size);
            if (n <= 0) {
                vnode_unref(vn);
                kfree(buf);
                kfree(ep);
                regs->eax = (uint32_t) -EIO;
                break;
            }

            /* Support #! shebang scripts */
            if (n >= 4 && buf[0] == '#' && buf[1] == '!') {
                int p = 2;
                while (p < n && (buf[p] == ' ' || buf[p] == '\t')) p++;
                char interp[128];
                int il = 0;
                while (p < n && buf[p] != '\n' && buf[p] != '\r' && buf[p] != ' ' && buf[p] != '\t' && il < 127) {
                    interp[il++] = (char)buf[p++];
                }
                interp[il] = '\0';
                kfree(buf);
                vnode_unref(vn);
                vn = 0;
                if (il == 0 || vfs_resolve_path_ex(interp, me->cwd, &vn, 1, ep->resolved_canon, sizeof(ep->resolved_canon)) != 0 || !vn) {
                    kfree(ep);
                    regs->eax = (uint32_t) -ENOENT;
                    break;
                }
                if (me->agent_id > 0 && ep->resolved_canon[0]) {
                    int aerr = agent_check_access(me, CAP_EXEC, ep->resolved_canon, "exec_interp");
                    if (aerr < 0) {
                        vnode_unref(vn);
                        kfree(ep);
                        regs->eax = (uint32_t) aerr;
                        break;
                    }
                }
                if (argc < 31) {
                    for (int a = argc; a >= 1; a--) {
                        for (int c = 0; c <= NAME_MAX; c++) ep->kargv_buf[a][c] = ep->kargv_buf[a - 1][c];
                        kargv[a] = ep->kargv_buf[a];
                    }
                    int c = 0;
                    while (ep->kname[c] && c < NAME_MAX) { ep->kargv_buf[1][c] = ep->kname[c]; c++; }
                    ep->kargv_buf[1][c] = '\0';
                    c = 0;
                    while (interp[c] && c < NAME_MAX) { ep->kargv_buf[0][c] = interp[c]; c++; }
                    ep->kargv_buf[0][c] = '\0';
                    kargv[0] = ep->kargv_buf[0];
                    argc++;
                    kargv[argc] = 0;
                }
                alloc_size = ((vn->size + 511) / 512) * 512;
                buf = (uint8_t*) kmalloc(alloc_size);
                if (!buf) {
                    vnode_unref(vn);
                    kfree(ep);
                    regs->eax = (uint32_t) -ENOMEM;
                    break;
                }
                n = vn->ops->read(vn, 0, buf, vn->size);
                if (n <= 0) {
                    vnode_unref(vn);
                    kfree(buf);
                    kfree(ep);
                    regs->eax = (uint32_t) -EIO;
                    break;
                }
            }

            address_space_t new_as = vmm_create_address_space();
            if (!new_as.pml4) {
                vnode_unref(vn);
                kfree(buf);
                kfree(ep);
                regs->eax = (uint64_t) -ENOMEM;
                break;
            }

            me = scheduler_current();
            me->aslr_mmap_offset  = ((uint64_t)(random_u32() & 0x1FFFFULL)) << 12;
            me->aslr_stack_offset = (((uint64_t)(random_u32() & 0x7FFULL)) << 12) | (((uint64_t)(random_u32() & 0x1FULL)) << 4);
            me->aslr_brk_offset   = ((uint64_t)(random_u32() & 0x1FFFFULL)) << 12;
            me->aslr_pie_offset   = ((uint64_t)(random_u32() & 0x3FFFFULL)) << 12;
            me->fs_base = 0;
            me->gs_base = 0;
            me->clear_child_tid = 0;

            uint64_t entry, stack_top;
            vma_t* new_vmas = NULL;
            if (elf_load_into(buf, (uint32_t) n, vn, me, &new_as, argc, kargv, envc, kenvp, &entry, &stack_top, &new_vmas) != 0) {
                vnode_unref(vn);
                kfree(buf);
                vmm_destroy_address_space(&new_as);
                kfree(ep);
                regs->eax = (uint32_t) -ENOEXEC;
                break;
            }
            vnode_unref(vn);
            kfree(buf);

            /* Close file descriptors marked with FD_CLOEXEC */
            for (int fd = 0; fd < MAX_FDS; fd++) {
                if (me->fds[fd] && (me->fd_flags[fd] & FD_CLOEXEC)) {
                    open_file_t* of = me->fds[fd];
                    me->fds[fd] = 0;
                    me->fd_flags[fd] = 0;
                    vfs_close(of);
                }
            }

            if (me->vma_list) {
                vma_free_list(me->vma_list);
            }
            me->vma_list = new_vmas;

            address_space_t old_as = me->as;
            me->as = new_as;
            me->entry_point = entry;
            me->user_stack_top = stack_top;
            me->heap_end = HEAP_BASE + me->aslr_brk_offset;
            me->heap_mapped_up_to = me->heap_end;

            const char* ex_src = ep->resolved_canon[0] ? ep->resolved_canon : ep->kname;
            int exi = 0;
            while (ex_src[exi] && exi < 255) { me->exe_path[exi] = ex_src[exi]; exi++; }
            me->exe_path[exi] = '\0';

            int is_musl = 0;
            for (int k = 0; ep->kname[k]; k++) {
                if (ep->kname[k] == 'm' && ep->kname[k+1] == 'u' && ep->kname[k+2] == 's' && ep->kname[k+3] == 'l') {
                    is_musl = 1;
                    break;
                }
            }
            if (is_musl) {
                uint64_t target = 0xffff800000395af0ULL;
                asm volatile ("mov %0, %%dr0" : : "r"(target));
                asm volatile ("mov %0, %%dr7" : : "r"(0x000D0001ULL));
                serial_printf("[watchpoint] armed DR0 on %p for musltest\n", (void*)target);
            }

            const char* base = ep->kname;
            for (int k = 0; ep->kname[k]; k++) {
                if (ep->kname[k] == '/') base = &ep->kname[k + 1];
            }
            int i = 0; for (; base[i] && i < 31; i++) me->name[i] = base[i]; me->name[i] = '\0';
            kfree(ep);

            for (int sig = 1; sig < NSIG; sig++) {
                if (me->sig_actions[sig].sa_handler != SIG_IGN) {
                    me->sig_actions[sig].sa_handler = SIG_DFL;
                    me->sig_actions[sig].sa_mask = 0;
                    me->sig_actions[sig].sa_flags = 0;
                    me->sig_actions[sig].sa_restorer = 0;
                }
            }
            wrmsr64(MSR_FS_BASE, 0);

            vmm_switch(&me->as);
            vmm_destroy_address_space(&old_as);

            fpu_init_proc(me);
            fpu_restore(me);

            enter_usermode(entry, stack_top); /* never returns */
            break;
        }

        case SYS_WAITPID: {
            int pid = (int) regs->ebx;
            int* user_status = (int*) regs->ecx;
            int options = (int) regs->edx;

            if (user_status) {
                int err = validate_user_buffer(user_status, sizeof(int), 1);
                if (err != 0) {
                    regs->eax = (uint32_t) err;
                    break;
                }
            }

            process_t* me = scheduler_current();

            for (;;) {
                int has_children = 0;
                process_t* zombie = 0;
                process_t* stopped_child = 0;

                for (int i = 0; i < MAX_PROCESSES; i++) {
                    process_t* child = process_table_entry(i);
                    if (!child || child->state == PROC_UNUSED) continue;
                    if (child->ppid != me->pid) continue;

                    int matches = 0;
                    if (pid == -1) {
                        matches = 1;
                    } else if (pid > 0) {
                        matches = (child->pid == pid);
                    } else if (pid == 0) {
                        matches = (child->pgid == me->pgid);
                    } else /* pid < -1 */ {
                        matches = (child->pgid == -pid);
                    }

                    if (matches) {
                        has_children = 1;
                        if (child->state == PROC_ZOMBIE) {
                            zombie = child;
                            break;
                        }
                        if ((options & 2 /* WUNTRACED */) && child->is_stopped && !child->stopped_reported) {
                            stopped_child = child;
                            break;
                        }
                    }
                }

                if (!has_children) {
                    regs->eax = (uint32_t) -ECHILD;
                    break;
                }

                if (stopped_child) {
                    stopped_child->stopped_reported = 1;
                    if (user_status) {
                        *user_status = (stopped_child->stop_sig << 8) | 0x7F;
                    }
                    regs->eax = (uint32_t) stopped_child->pid;
                    break;
                }

                if (zombie) {
                    int reaped_pid = zombie->pid;
                    int exit_code = zombie->exit_code;
                    if (user_status) {
                        int sig = zombie->term_sig;
                        if (sig == 0 && exit_code >= 128 && exit_code < 160) {
                            sig = exit_code - 128;
                        }
                        *user_status = ((exit_code & 0xff) << 8) | (sig & 0x7f);
                    }
                    process_destroy(zombie);
                    regs->eax = (uint32_t) reaped_pid;
                    break;
                }

                if (options & WNOHANG) {
                    regs->eax = 0;
                    break;
                }

                if (signal_has_deliverable(me)) {
                    regs->eax = (uint32_t) -EINTR;
                    break;
                }

                scheduler_wait_for(pid);
            }
            break;
        }

        case SYS_BRK: {
            uint64_t new_break = (uint64_t) regs->ebx;
            process_t* me = scheduler_current();
            uint64_t heap_base = HEAP_BASE + me->aslr_brk_offset;
            uint64_t cur_break = me->heap_end;
            if (cur_break < heap_base) cur_break = heap_base;

            /* Linux brk(0) returns current break */
            if (new_break == 0 || new_break == cur_break) {
                regs->eax = cur_break;
                break;
            }

            if (new_break < heap_base || new_break > (heap_base + 2ULL * 1024ULL * 1024ULL * 1024ULL)) {
                /* Failure: return current break unchanged */
                regs->eax = cur_break;
                break;
            }

            if (new_break < cur_break) {
                /* Shrinking: unmap pages above new_break */
                uint64_t old_page = (cur_break + 4095ULL) & ~4095ULL;
                uint64_t new_page = (new_break + 4095ULL) & ~4095ULL;
                for (uint64_t va = new_page; va < old_page; va += 4096) {
                    vmm_unmap_user_page(&me->as, va);
                }
            }

            me->heap_end = new_break;
            if (me->vma_list) {
                vma_t* v = me->vma_list;
                while (v) {
                    if (v->flags & VMA_FLAG_HEAP) {
                        v->end = (new_break + 4095ULL) & ~4095ULL;
                        break;
                    }
                    v = v->next;
                }
            }
            regs->eax = new_break;
            break;
        }

        case SYS_SYNC:
        case __NR_fdatasync: {
            regs->eax = (uint32_t) bcache_sync();
            break;
        }

        case __NR_readv: {
            int fd = (int) regs->ebx;
            const struct iovec* uiov = (const struct iovec*) regs->ecx;
            int iovcnt = (int) regs->edx;
            if (iovcnt < 0 || iovcnt > 1024) { regs->eax = (uint32_t) -EINVAL; break; }
            if (iovcnt == 0) { regs->eax = 0; break; }
            int err = validate_user_buffer(uiov, (uint64_t)iovcnt * sizeof(struct iovec), 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            int64_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (uiov[i].iov_len == 0) continue;
                err = validate_user_buffer(uiov[i].iov_base, uiov[i].iov_len, 1);
                if (err != 0) {
                    if (total == 0) total = err;
                    break;
                }
                int n = vfs_read(me->fds[fd], uiov[i].iov_base, (uint32_t)uiov[i].iov_len);
                if (n < 0) {
                    if (total == 0) total = n;
                    break;
                }
                total += n;
                if ((size_t)n < uiov[i].iov_len) break;
            }
            regs->eax = (uint64_t) total;
            break;
        }

        case __NR_writev: {
            int fd = (int) regs->ebx;
            const struct iovec* uiov = (const struct iovec*) regs->ecx;
            int iovcnt = (int) regs->edx;
            if (iovcnt < 0 || iovcnt > 1024) { regs->eax = (uint32_t) -EINVAL; break; }
            if (iovcnt == 0) { regs->eax = 0; break; }
            int err = validate_user_buffer(uiov, (uint64_t)iovcnt * sizeof(struct iovec), 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            int64_t total = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (uiov[i].iov_len == 0) continue;
                err = validate_user_buffer(uiov[i].iov_base, uiov[i].iov_len, 0);
                if (err != 0) {
                    if (total == 0) total = err;
                    break;
                }
                int n = vfs_write(me->fds[fd], uiov[i].iov_base, (uint32_t)uiov[i].iov_len);
                if (n < 0) {
                    if (total == 0) total = n;
                    break;
                }
                total += n;
                if ((size_t)n < uiov[i].iov_len) break;
            }
            regs->eax = (uint64_t) total;
            break;
        }

        case __NR_pread64: {
            int fd = (int) regs->ebx;
            void* buf = (void*) regs->ecx;
            uint32_t count = (uint32_t) regs->edx;
            int64_t pos = (int64_t) regs->esi;
            if (pos < 0) { regs->eax = (uint32_t) -EINVAL; break; }
            int err = validate_user_buffer(buf, count, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) { regs->eax = (uint32_t) -EBADF; break; }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_VNODE || !of->vnode) { regs->eax = (uint32_t) -ESPIPE; break; }
            uint32_t old_off = of->offset;
            of->offset = (uint32_t) pos;
            int n = vfs_read(of, buf, count);
            of->offset = old_off;
            regs->eax = (uint32_t) n;
            break;
        }

        case __NR_pwrite64: {
            int fd = (int) regs->ebx;
            const void* buf = (const void*) regs->ecx;
            uint32_t count = (uint32_t) regs->edx;
            int64_t pos = (int64_t) regs->esi;
            if (pos < 0) { regs->eax = (uint32_t) -EINVAL; break; }
            int err = validate_user_buffer(buf, count, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) { regs->eax = (uint32_t) -EBADF; break; }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_VNODE || !of->vnode) { regs->eax = (uint32_t) -ESPIPE; break; }
            uint32_t old_off = of->offset;
            of->offset = (uint32_t) pos;
            int n = vfs_write(of, buf, count);
            of->offset = old_off;
            regs->eax = (uint32_t) n;
            break;
        }

        case __NR_sendfile: {
            int out_fd = (int) regs->ebx;
            int in_fd = (int) regs->ecx;
            int64_t* offset_ptr = (int64_t*) regs->edx;
            uint32_t count = (uint32_t) regs->esi;
            process_t* me = scheduler_current();
            if (out_fd < 0 || out_fd >= MAX_FDS || !me->fds[out_fd] ||
                in_fd < 0 || in_fd >= MAX_FDS || !me->fds[in_fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            if (offset_ptr) {
                int err = validate_user_buffer(offset_ptr, sizeof(int64_t), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
            }
            uint8_t kbuf[512];
            uint32_t total = 0;
            uint32_t saved_off = me->fds[in_fd]->offset;
            if (offset_ptr) me->fds[in_fd]->offset = (uint32_t)(*offset_ptr);
            while (total < count) {
                uint32_t chunk = (count - total > sizeof(kbuf)) ? sizeof(kbuf) : (count - total);
                int r = vfs_read(me->fds[in_fd], kbuf, chunk);
                if (r <= 0) break;
                int w = vfs_write(me->fds[out_fd], kbuf, (uint32_t)r);
                if (w <= 0) break;
                total += (uint32_t)w;
                if (w < r) break;
            }
            if (offset_ptr) {
                *offset_ptr = (int64_t)me->fds[in_fd]->offset;
                me->fds[in_fd]->offset = saved_off;
            }
            regs->eax = total;
            break;
        }

        case SYS_LSEEK: {
            int fd = (int) regs->ebx;
            int offset = (int) regs->ecx;
            int whence = (int) regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            int ret = vfs_lseek(me->fds[fd], offset, whence);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_STAT: {
            const char* path = (const char*) regs->ebx;
            struct linux_stat64* u_ls = (struct linux_stat64*) regs->ecx;
            regs->eax = (uint32_t) do_sys_statat(AT_FDCWD, path, u_ls, 0);
            break;
        }

        case __NR_lstat: {
            const char* path = (const char*) regs->ebx;
            struct linux_stat64* u_ls = (struct linux_stat64*) regs->ecx;
            regs->eax = (uint32_t) do_sys_statat(AT_FDCWD, path, u_ls, AT_SYMLINK_NOFOLLOW);
            break;
        }

        case SYS_NEWFSTATAT: {
            int dirfd = (int) regs->ebx;
            const char* path = (const char*) regs->ecx;
            struct linux_stat64* u_ls = (struct linux_stat64*) regs->edx;
            int flags = (int) regs->esi;
            regs->eax = (uint32_t) do_sys_statat(dirfd, path, u_ls, flags);
            break;
        }

        case SYS_FSTAT: {
            int fd = (int) regs->ebx;
            struct linux_stat64* u_ls = (struct linux_stat64*) regs->ecx;
            regs->eax = (uint32_t) do_sys_fstat(fd, u_ls);
            break;
        }

        case __NR_statx: {
            regs->eax = (uint32_t) -ENOSYS;
            break;
        }

        case SYS_TRUNCATE: {
            const char* path = (const char*) regs->ebx;
            int32_t length = (int32_t) regs->ecx;

            if (length < 0) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            char full_path[PATH_MAX];
            vfs_path_canonical(path, me->cwd, full_path, sizeof(full_path));
            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, "truncate");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }

            vnode_t* vn = 0;
            char resolved_canon[PATH_MAX];
            resolved_canon[0] = '\0';
            int ret = vfs_resolve_path_ex(path, me->cwd, &vn, 1, resolved_canon, sizeof(resolved_canon));
            if (ret != 0 || !vn) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }

            if (me->agent_id > 0 && resolved_canon[0]) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, resolved_canon, "truncate");
                if (aerr < 0) {
                    vnode_unref(vn);
                    regs->eax = (uint32_t) aerr;
                    break;
                }
            }

            ret = vfs_truncate(vn, (uint32_t) length);
            vnode_unref(vn);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_FTRUNCATE: {
            int fd = (int) regs->ebx;
            int32_t length = (int32_t) regs->ecx;

            if (length < 0) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_VNODE || !of->vnode) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            int acc = of->flags & (O_WRONLY | O_RDWR);
            if (!acc) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            int ret = vfs_truncate(of->vnode, (uint32_t) length);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_GETDENTS:
        case __NR_getdents: {
            int fd = (int) regs->ebx;
            void* dirp = (void*) regs->ecx;
            uint32_t count = (uint32_t) regs->edx;

            if (count < 24) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            int err = validate_user_buffer(dirp, count, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            uint32_t bytes_written = 0;
            uint8_t* u_buf = (uint8_t*) dirp;

            while (bytes_written < count) {
                struct dirent k_ent;
                memset(&k_ent, 0, sizeof(k_ent));
                int ret = vfs_readdir(me->fds[fd], &k_ent);
                if (ret <= 0) {
                    if (ret < 0 && bytes_written == 0) {
                        regs->eax = (uint32_t) ret;
                        break;
                    }
                    regs->eax = bytes_written;
                    break;
                }

                if (me->agent_id > 0) {
                    char child_canon[PATH_MAX];
                    const char* base_p = me->fds[fd]->path[0] ? me->fds[fd]->path : me->cwd;
                    vfs_path_canonical(k_ent.d_name, base_p, child_canon, sizeof(child_canon));
                    if (agent_check_access(me, CAP_FS_READ, child_canon, "getdents") != 0) {
                        continue; /* Skip entry outside caps: listing must not leak names outside caps */
                    }
                }

                size_t name_len = kstrlen(k_ent.d_name);
                uint16_t reclen = (uint16_t)(((19 + name_len + 1) + 7) & ~7);
                if (bytes_written + reclen > count) {
                    if (bytes_written == 0) {
                        regs->eax = (uint32_t) -EINVAL;
                    } else {
                        if (me->fds[fd]->offset > 0) me->fds[fd]->offset--;
                        regs->eax = bytes_written;
                    }
                    break;
                }

                uint8_t dtype = (k_ent.d_type == 2) ? 4 : 8; /* DT_DIR = 4, DT_REG = 8 */
                if (dtype == 8 && me->fds[fd]->vnode && me->fds[fd]->vnode->ops && me->fds[fd]->vnode->ops->lookup) {
                    vnode_t* ch_vn = 0;
                    if (me->fds[fd]->vnode->ops->lookup(me->fds[fd]->vnode, k_ent.d_name, &ch_vn) == 0 && ch_vn) {
                        if (vfs_is_symlink(ch_vn, 0, 0) > 0) dtype = 10; /* DT_LNK = 10 */
                        vnode_unref(ch_vn);
                    }
                }

                uint8_t rec_buf[512];
                memset(rec_buf, 0, reclen);
                struct linux_dirent64* ld = (struct linux_dirent64*) rec_buf;
                ld->d_ino = k_ent.d_ino ? k_ent.d_ino : 1;
                ld->d_off = me->fds[fd]->offset;
                ld->d_reclen = reclen;
                ld->d_type = dtype;
                memcpy(ld->d_name, k_ent.d_name, name_len + 1);

                if (copy_to_user(u_buf + bytes_written, rec_buf, reclen) != 0) {
                    if (bytes_written == 0) regs->eax = (uint32_t) -EFAULT;
                    else regs->eax = bytes_written;
                    break;
                }

                bytes_written += reclen;
                regs->eax = bytes_written;
                break;
            }
            break;
        }

        case SYS_MKDIR:
        case __NR_mkdirat: {
            int dirfd = (regs->eax == __NR_mkdirat) ? (int) regs->ebx : AT_FDCWD;
            const char* path = (const char*) ((regs->eax == __NR_mkdirat) ? regs->ecx : regs->ebx);
            int mode = (int) ((regs->eax == __NR_mkdirat) ? regs->edx : regs->ecx);
            (void) mode;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            const char* base_cwd = me->cwd;
            err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char full_path[PATH_MAX];
            vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, "mkdir");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            vnode_t* parent = 0;
            char child[NAME_MAX + 1];
            char parent_canon[PATH_MAX];
            parent_canon[0] = '\0';
            int ret = vfs_resolve_parent_ex(path, base_cwd, &parent, child, sizeof(child), parent_canon, sizeof(parent_canon));
            if (ret != 0 || !parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (me->agent_id > 0 && parent_canon[0]) {
                char target_canon[PATH_MAX];
                vfs_path_canonical(child, parent_canon, target_canon, sizeof(target_canon));
                int aerr = agent_check_access(me, CAP_FS_WRITE, target_canon, "mkdir");
                if (aerr < 0) { vnode_unref(parent); regs->eax = (uint32_t) aerr; break; }
            }
            if (!parent->ops || !parent->ops->mkdir) {
                vnode_unref(parent);
                regs->eax = (uint32_t) -ENOSYS;
                break;
            }
            vnode_t* out = 0;
            ret = parent->ops->mkdir(parent, child, &out);
            vnode_unref(parent);
            if (out) vnode_unref(out);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_RMDIR:
        case SYS_UNLINK:
        case SYS_UNLINKAT: {
            int dirfd = (regs->eax == SYS_UNLINKAT) ? (int) regs->ebx : AT_FDCWD;
            const char* path = (const char*) ((regs->eax == SYS_UNLINKAT) ? regs->ecx : regs->ebx);
            int uflags = (regs->eax == SYS_UNLINKAT) ? (int) regs->edx : ((regs->eax == SYS_RMDIR) ? AT_REMOVEDIR : 0);

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            const char* base_cwd = me->cwd;
            err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char full_path[PATH_MAX];
            vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, (uflags & AT_REMOVEDIR) ? "rmdir" : "unlink");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            vnode_t* parent = 0;
            char child[NAME_MAX + 1];
            char parent_canon[PATH_MAX];
            parent_canon[0] = '\0';
            int ret = vfs_resolve_parent_ex(path, base_cwd, &parent, child, sizeof(child), parent_canon, sizeof(parent_canon));
            if (ret != 0 || !parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (me->agent_id > 0 && parent_canon[0]) {
                char target_canon[PATH_MAX];
                vfs_path_canonical(child, parent_canon, target_canon, sizeof(target_canon));
                int aerr = agent_check_access(me, CAP_FS_WRITE, target_canon, (uflags & AT_REMOVEDIR) ? "rmdir" : "unlink");
                if (aerr < 0) { vnode_unref(parent); regs->eax = (uint32_t) aerr; break; }
            }
            if (uflags & AT_REMOVEDIR) {
                if (!parent->ops || !parent->ops->rmdir) {
                    vnode_unref(parent);
                    regs->eax = (uint32_t) -ENOSYS;
                    break;
                }
                ret = parent->ops->rmdir(parent, child);
            } else {
                if (!parent->ops || !parent->ops->unlink) {
                    vnode_unref(parent);
                    regs->eax = (uint32_t) -ENOSYS;
                    break;
                }
                ret = parent->ops->unlink(parent, child);
            }
            vnode_unref(parent);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_RENAME:
        case SYS_RENAMEAT:
        case SYS_RENAMEAT2: {
            int olddirfd = (regs->eax == SYS_RENAME) ? AT_FDCWD : (int) regs->ebx;
            const char* oldpath = (const char*) ((regs->eax == SYS_RENAME) ? regs->ebx : regs->ecx);
            int newdirfd = (regs->eax == SYS_RENAME) ? AT_FDCWD : (int) regs->edx;
            const char* newpath = (const char*) ((regs->eax == SYS_RENAME) ? regs->ecx : regs->esi);

            int err = validate_user_string(oldpath);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = validate_user_string(newpath);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            const char* old_base = me->cwd;
            const char* new_base = me->cwd;
            err = resolve_dirfd_base(me, olddirfd, oldpath, &old_base);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = resolve_dirfd_base(me, newdirfd, newpath, &new_base);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char old_full[PATH_MAX], new_full[PATH_MAX];
            vfs_path_canonical(oldpath, old_base, old_full, sizeof(old_full));
            vfs_path_canonical(newpath, new_base, new_full, sizeof(new_full));

            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, old_full, "rename_old");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
                aerr = agent_check_access(me, CAP_FS_WRITE, new_full, "rename_new");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            vnode_t *old_parent = 0, *new_parent = 0;
            char old_child[NAME_MAX + 1], new_child[NAME_MAX + 1];
            char old_pcanon[PATH_MAX], new_pcanon[PATH_MAX];
            old_pcanon[0] = '\0'; new_pcanon[0] = '\0';
            int ret = vfs_resolve_parent_ex(oldpath, old_base, &old_parent, old_child, sizeof(old_child), old_pcanon, sizeof(old_pcanon));
            if (ret != 0 || !old_parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            ret = vfs_resolve_parent_ex(newpath, new_base, &new_parent, new_child, sizeof(new_child), new_pcanon, sizeof(new_pcanon));
            if (ret != 0 || !new_parent) {
                vnode_unref(old_parent);
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (me->agent_id > 0) {
                if (old_pcanon[0]) {
                    char tc[PATH_MAX];
                    vfs_path_canonical(old_child, old_pcanon, tc, sizeof(tc));
                    int aerr = agent_check_access(me, CAP_FS_WRITE, tc, "rename_old");
                    if (aerr < 0) { vnode_unref(old_parent); vnode_unref(new_parent); regs->eax = (uint32_t) aerr; break; }
                }
                if (new_pcanon[0]) {
                    char tc[PATH_MAX];
                    vfs_path_canonical(new_child, new_pcanon, tc, sizeof(tc));
                    int aerr = agent_check_access(me, CAP_FS_WRITE, tc, "rename_new");
                    if (aerr < 0) { vnode_unref(old_parent); vnode_unref(new_parent); regs->eax = (uint32_t) aerr; break; }
                }
            }
            if (!old_parent->ops || !old_parent->ops->rename) {
                vnode_unref(old_parent);
                vnode_unref(new_parent);
                regs->eax = (uint32_t) -ENOSYS;
                break;
            }
            ret = old_parent->ops->rename(old_parent, old_child, new_parent, new_child);
            vnode_unref(old_parent);
            vnode_unref(new_parent);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_SYMLINK: {
            const char* target = (const char*) regs->ebx;
            const char* linkpath = (const char*) regs->ecx;
            regs->eax = (uint32_t) do_sys_symlinkat(target, AT_FDCWD, linkpath);
            break;
        }

        case SYS_SYMLINKAT: {
            const char* target = (const char*) regs->ebx;
            int newdirfd = (int) regs->ecx;
            const char* linkpath = (const char*) regs->edx;
            regs->eax = (uint32_t) do_sys_symlinkat(target, newdirfd, linkpath);
            break;
        }

        case SYS_READLINK: {
            const char* path = (const char*) regs->ebx;
            char* buf = (char*) regs->ecx;
            size_t bufsiz = (size_t) regs->edx;
            regs->eax = (uint32_t) do_sys_readlinkat(AT_FDCWD, path, buf, bufsiz);
            break;
        }

        case SYS_READLINKAT: {
            int dirfd = (int) regs->ebx;
            const char* path = (const char*) regs->ecx;
            char* buf = (char*) regs->edx;
            size_t bufsiz = (size_t) regs->esi;
            regs->eax = (uint32_t) do_sys_readlinkat(dirfd, path, buf, bufsiz);
            break;
        }

        case __NR_access:
        case SYS_FACCESSAT:
        case __NR_faccessat2: {
            int dirfd = (regs->eax == __NR_access) ? AT_FDCWD : (int) regs->ebx;
            const char* path = (const char*) ((regs->eax == __NR_access) ? regs->ebx : regs->ecx);
            int amode = (int) ((regs->eax == __NR_access) ? regs->ecx : regs->edx);

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            const char* base_cwd = me->cwd;
            err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char full_path[PATH_MAX];
            vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
            if (me->agent_id > 0) {
                uint32_t cap = (amode & 2 /* W_OK */) ? CAP_FS_WRITE : CAP_FS_READ;
                int aerr = agent_check_access(me, cap, full_path, "access");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }

            vnode_t* vn = 0;
            char resolved_canon[PATH_MAX];
            resolved_canon[0] = '\0';
            int ret = vfs_resolve_path_ex(path, base_cwd, &vn, 1, resolved_canon, sizeof(resolved_canon));
            if (ret != 0 || !vn) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            vnode_unref(vn);
            if (me->agent_id > 0 && resolved_canon[0]) {
                uint32_t cap = (amode & 2 /* W_OK */) ? CAP_FS_WRITE : CAP_FS_READ;
                int aerr = agent_check_access(me, cap, resolved_canon, "access");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            regs->eax = 0;
            break;
        }

        case SYS_CHMOD:
        case SYS_FCHMODAT:
        case SYS_CHOWN:
        case __NR_lchown:
        case __NR_fchownat:
        case SYS_UTIMENSAT: {
            int has_dirfd = (regs->eax == SYS_FCHMODAT || regs->eax == __NR_fchownat || regs->eax == SYS_UTIMENSAT);
            int dirfd = has_dirfd ? (int) regs->ebx : AT_FDCWD;
            const char* path = (const char*) (has_dirfd ? regs->ecx : regs->ebx);

            if (regs->eax == SYS_UTIMENSAT && !path) {
                regs->eax = 0;
                break;
            }
            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            const char* base_cwd = me->cwd;
            err = resolve_dirfd_base(me, dirfd, path, &base_cwd);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char full_path[PATH_MAX];
            vfs_path_canonical(path, base_cwd, full_path, sizeof(full_path));
            if (me->agent_id > 0) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, full_path, "chmod_chown");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            vnode_t* vn = 0;
            char resolved_canon[PATH_MAX];
            resolved_canon[0] = '\0';
            int ret = vfs_resolve_path_ex(path, base_cwd, &vn, (regs->eax == __NR_lchown) ? 0 : 1, resolved_canon, sizeof(resolved_canon));
            if (ret != 0 || !vn) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            vnode_unref(vn);
            if (me->agent_id > 0 && resolved_canon[0]) {
                int aerr = agent_check_access(me, CAP_FS_WRITE, resolved_canon, "chmod_chown");
                if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
            }
            regs->eax = 0;
            break;
        }

        case SYS_FCHMOD:
        case __NR_fchown: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            regs->eax = 0;
            break;
        }

        case SYS_UMASK: {
            process_t* me = scheduler_current();
            uint32_t old = me->umask_val;
            me->umask_val = (uint32_t) regs->ebx & 0777U;
            regs->eax = old;
            break;
        }

        case SYS_CHDIR: {
            const char* path = (const char*) regs->ebx;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* vn = 0;
            char resolved_canon[PATH_MAX];
            resolved_canon[0] = '\0';
            int ret = vfs_resolve_path_ex(path, me->cwd, &vn, 1, resolved_canon, sizeof(resolved_canon));
            if (ret != 0 || !vn) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (vn->type != VNODE_DIR) {
                vnode_unref(vn);
                regs->eax = (uint32_t) -ENOTDIR;
                break;
            }
            vnode_unref(vn);

            if (resolved_canon[0]) {
                int ci = 0;
                while (resolved_canon[ci] && ci < (int)sizeof(me->cwd) - 1) {
                    me->cwd[ci] = resolved_canon[ci];
                    ci++;
                }
                me->cwd[ci] = '\0';
            } else {
                vfs_path_canonical(path, me->cwd, me->cwd, sizeof(me->cwd));
            }
            regs->eax = 0;
            break;
        }

        case __NR_fchdir: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_VNODE || !of->vnode || of->vnode->type != VNODE_DIR) {
                regs->eax = (uint32_t) -ENOTDIR;
                break;
            }
            if (of->path[0]) {
                int ci = 0;
                while (of->path[ci] && ci < (int)sizeof(me->cwd) - 1) {
                    me->cwd[ci] = of->path[ci];
                    ci++;
                }
                me->cwd[ci] = '\0';
            }
            regs->eax = 0;
            break;
        }

        case SYS_GETCWD: {
            char* buf = (char*) regs->ebx;
            uint32_t size = regs->ecx;

            if (size == 0) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            int err = validate_user_buffer(buf, size, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            uint32_t len = 0;
            while (me->cwd[len]) len++;
            if (size < len + 1) {
                regs->eax = (uint32_t) -ERANGE;
                break;
            }
            for (uint32_t i = 0; i <= len; i++) {
                buf[i] = me->cwd[i];
            }
            regs->eax = (uint64_t) (uintptr_t) buf;
            break;
        }

        case SYS_DUP: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            int new_fd = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!me->fds[i]) {
                    new_fd = i;
                    break;
                }
            }
            if (new_fd < 0) {
                regs->eax = (uint32_t) -EMFILE;
                break;
            }
            me->fds[new_fd] = me->fds[fd];
            me->fd_flags[new_fd] = 0;
            open_file_ref(me->fds[new_fd]);
            regs->eax = (uint32_t) new_fd;
            break;
        }

        case SYS_DUP2:
        case SYS_DUP3: {
            int oldfd = (int) regs->ebx;
            int newfd = (int) regs->ecx;
            int dflags = (regs->eax == SYS_DUP3) ? (int) regs->edx : 0;
            process_t* me = scheduler_current();
            if (oldfd < 0 || oldfd >= MAX_FDS || !me->fds[oldfd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            if (newfd < 0 || newfd >= MAX_FDS) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            if (oldfd == newfd) {
                regs->eax = (regs->eax == SYS_DUP3) ? (uint32_t) -EINVAL : (uint32_t) newfd;
                break;
            }
            if (me->fds[newfd]) {
                open_file_t* old_of = me->fds[newfd];
                me->fds[newfd] = 0;
                me->fd_flags[newfd] = 0;
                vfs_close(old_of);
            }
            me->fds[newfd] = me->fds[oldfd];
            me->fd_flags[newfd] = (dflags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            open_file_ref(me->fds[newfd]);
            regs->eax = (uint32_t) newfd;
            break;
        }

        case SYS_FCNTL: {
            int fd = (int) regs->ebx;
            int cmd = (int) regs->ecx;
            int arg = (int) regs->edx;
            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            if (cmd == F_GETFD) {
                regs->eax = (uint32_t) me->fd_flags[fd];
            } else if (cmd == F_SETFD) {
                me->fd_flags[fd] = (uint8_t) (arg & FD_CLOEXEC);
                regs->eax = 0;
            } else if (cmd == F_GETFL) {
                regs->eax = (uint32_t) me->fds[fd]->flags;
            } else if (cmd == F_SETFL) {
                int allowed = O_APPEND | O_NONBLOCK | O_NONBLOCK_LINUX;
                me->fds[fd]->flags = (me->fds[fd]->flags & ~allowed) | (arg & allowed);
                regs->eax = 0;
            } else if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
                if (arg < 0 || arg >= MAX_FDS) {
                    regs->eax = (uint32_t) -EINVAL;
                    break;
                }
                int new_fd = -1;
                for (int i = arg; i < MAX_FDS; i++) {
                    if (!me->fds[i]) {
                        new_fd = i;
                        break;
                    }
                }
                if (new_fd < 0) {
                    regs->eax = (uint32_t) -EMFILE;
                } else {
                    me->fds[new_fd] = me->fds[fd];
                    me->fd_flags[new_fd] = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                    open_file_ref(me->fds[new_fd]);
                    regs->eax = (uint32_t) new_fd;
                }
            } else {
                regs->eax = (uint32_t) -EINVAL;
            }
            break;
        }

        case __NR_madvise: {
            regs->eax = 0;
            break;
        }

        case __NR_arch_prctl: {
            int code = (int) regs->ebx;
            uint64_t addr = regs->ecx;
            process_t* me = scheduler_current();
            if (!me) { regs->eax = (uint64_t) -ESRCH; break; }
            if (code == 0x1002) { /* ARCH_SET_FS */
                if (addr >= 0x0000800000000000ULL) {
                    regs->eax = (uint64_t) -EPERM;
                    break;
                }
                me->fs_base = addr;
                wrmsr64(MSR_FS_BASE, addr);
                regs->eax = 0;
            } else if (code == 0x1003) { /* ARCH_GET_FS */
                uint64_t* uptr = (uint64_t*) addr;
                int err = validate_user_buffer(uptr, sizeof(uint64_t), 1);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                copy_to_user(uptr, &me->fs_base, sizeof(uint64_t));
                regs->eax = 0;
            } else if (code == 0x1001) { /* ARCH_SET_GS */
                if (addr >= 0x0000800000000000ULL) {
                    regs->eax = (uint64_t) -EPERM;
                    break;
                }
                me->gs_base = addr;
                regs->eax = 0;
            } else if (code == 0x1004) { /* ARCH_GET_GS */
                uint64_t* uptr = (uint64_t*) addr;
                int err = validate_user_buffer(uptr, sizeof(uint64_t), 1);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                copy_to_user(uptr, &me->gs_base, sizeof(uint64_t));
                regs->eax = 0;
            } else {
                regs->eax = (uint64_t) -EINVAL;
            }
            break;
        }

        case __NR_prctl: {
            int option = (int) regs->ebx;
            uint64_t arg2 = regs->ecx;
            process_t* me = scheduler_current();
            if (option == 15 && me) { /* PR_SET_NAME */
                const char* uname = (const char*) arg2;
                if (validate_user_string(uname) == 0) {
                    int i = 0;
                    while (uname[i] && i < 15) {
                        me->name[i] = uname[i];
                        i++;
                    }
                    me->name[i] = '\0';
                }
                regs->eax = 0;
            } else if (option == 16 && me) { /* PR_GET_NAME */
                char* uname = (char*) arg2;
                if (validate_user_buffer(uname, 16, 1) == 0) {
                    char buf[16];
                    memset(buf, 0, 16);
                    for (int i = 0; i < 15 && me->name[i]; i++) buf[i] = me->name[i];
                    copy_to_user(uname, buf, 16);
                    regs->eax = 0;
                } else {
                    regs->eax = (uint64_t) -EFAULT;
                }
            } else {
                regs->eax = 0;
            }
            break;
        }

        case __NR_set_tid_address: {
            int* tidptr = (int*) regs->ebx;
            process_t* me = scheduler_current();
            if (me) {
                me->clear_child_tid = tidptr;
                regs->eax = (uint64_t) me->pid;
            } else {
                regs->eax = 0;
            }
            break;
        }

        case __NR_set_robust_list: {
            regs->eax = 0;
            break;
        }

        case __NR_futex: {
            int* uaddr = (int*) regs->ebx;
            int futex_op = (int) regs->ecx;
            int val = (int) regs->edx;
            int cmd = futex_op & 0x7F;
            if (cmd == 0 || cmd == 9) { /* FUTEX_WAIT / FUTEX_WAIT_BITSET */
                int err = validate_user_buffer(uaddr, sizeof(int), 0);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                int cur_val = 0;
                copy_from_user(&cur_val, uaddr, sizeof(int));
                if (cur_val != val) {
                    regs->eax = (uint64_t) -EAGAIN;
                    break;
                }
                scheduler_yield();
                regs->eax = 0;
            } else if (cmd == 1 || cmd == 10) { /* FUTEX_WAKE / FUTEX_WAKE_BITSET */
                regs->eax = 0;
            } else {
                regs->eax = (uint64_t) -ENOSYS;
            }
            break;
        }

        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID: {
            process_t* me = scheduler_current();
            regs->eax = (me && me->agent_id > 0) ? (uint64_t)(1000 + me->agent_id) : 0;
            break;
        }

        case SYS_SETUID:
        case SYS_SETGID:
        case __NR_setreuid:
        case __NR_setregid:
        case __NR_setresuid:
        case __NR_setresgid:
        case __NR_setgroups: {
            process_t* me = scheduler_current();
            if (me && me->agent_id > 0) {
                regs->eax = (uint64_t) -EPERM;
            } else {
                regs->eax = 0;
            }
            break;
        }

        case __NR_getresuid:
        case __NR_getresgid: {
            uint32_t* r = (uint32_t*) regs->ebx;
            uint32_t* e = (uint32_t*) regs->ecx;
            uint32_t* s = (uint32_t*) regs->edx;
            process_t* me = scheduler_current();
            uint32_t id = (me && me->agent_id > 0) ? (uint32_t)(1000 + me->agent_id) : 0;
            if (r) {
                if (validate_user_buffer(r, sizeof(uint32_t), 1) != 0) { regs->eax = (uint64_t)-EFAULT; break; }
                copy_to_user(r, &id, sizeof(uint32_t));
            }
            if (e) {
                if (validate_user_buffer(e, sizeof(uint32_t), 1) != 0) { regs->eax = (uint64_t)-EFAULT; break; }
                copy_to_user(e, &id, sizeof(uint32_t));
            }
            if (s) {
                if (validate_user_buffer(s, sizeof(uint32_t), 1) != 0) { regs->eax = (uint64_t)-EFAULT; break; }
                copy_to_user(s, &id, sizeof(uint32_t));
            }
            regs->eax = 0;
            break;
        }

        case __NR_getgroups: {
            int size = (int) regs->ebx;
            uint32_t* list = (uint32_t*) regs->ecx;
            if (size < 0) { regs->eax = (uint64_t) -EINVAL; break; }
            if (size == 0) { regs->eax = 1; break; }
            if (validate_user_buffer(list, sizeof(uint32_t), 1) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
            process_t* me = scheduler_current();
            uint32_t gid = (me && me->agent_id > 0) ? (uint32_t)(1000 + me->agent_id) : 0;
            copy_to_user(list, &gid, sizeof(uint32_t));
            regs->eax = 1;
            break;
        }

        case __NR_getpgrp: {
            process_t* me = scheduler_current();
            regs->eax = (uint64_t)(me ? me->pgid : 0);
            break;
        }

        case __NR_tkill: {
            int tid = (int) regs->ebx;
            int sig = (int) regs->ecx;
            regs->eax = (uint64_t) sys_kill(tid, sig);
            break;
        }

        case __NR_tgkill: {
            int tgid = (int) regs->ebx;
            int tid  = (int) regs->ecx;
            int sig  = (int) regs->edx;
            (void)tgid;
            regs->eax = (uint64_t) sys_kill(tid, sig);
            break;
        }

        case __NR_sched_yield: {
            scheduler_yield();
            regs->eax = 0;
            break;
        }

        case SYS_UNAME: {
            void* ubuf = (void*) regs->ebx;
            struct linux_utsname {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
                char domainname[65];
            };
            int err = validate_user_buffer(ubuf, sizeof(struct linux_utsname), 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            struct linux_utsname kuts;
            memset(&kuts, 0, sizeof(kuts));
            memcpy(kuts.sysname, "MyOS", 5);
            memcpy(kuts.nodename, "myos", 5);
            memcpy(kuts.release, "6.1.0-myos", 11);
            memcpy(kuts.version, "#1 SMP PREEMPT_DYNAMIC x86_64", 30);
            memcpy(kuts.machine, "x86_64", 7);
            memcpy(kuts.domainname, "(none)", 7);
            copy_to_user(ubuf, &kuts, sizeof(kuts));
            regs->eax = 0;
            break;
        }

        case SYS_SYSINFO: {
            void* uinfo = (void*) regs->ebx;
            struct linux_sysinfo {
                int64_t  uptime;
                uint64_t loads[3];
                uint64_t totalram;
                uint64_t freeram;
                uint64_t sharedram;
                uint64_t bufferram;
                uint64_t totalswap;
                uint64_t freeswap;
                uint16_t procs;
                uint16_t pad;
                uint32_t pad2;
                uint64_t totalhigh;
                uint64_t freehigh;
                uint32_t mem_unit;
                char     _f[8];
            };
            int err = validate_user_buffer(uinfo, sizeof(struct linux_sysinfo), 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            struct linux_sysinfo ksi;
            memset(&ksi, 0, sizeof(ksi));
            ksi.uptime = timer_get_ticks() / 100u;
            ksi.totalram = (uint64_t) pmm_total_count() * 4096ULL;
            ksi.freeram = (uint64_t) pmm_free_frame_count() * 4096ULL;
            uint16_t nprocs = 0;
            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t* p = process_table_entry(i);
                if (p && p->state != PROC_UNUSED) nprocs++;
            }
            ksi.procs = nprocs;
            ksi.mem_unit = 1;
            copy_to_user(uinfo, &ksi, sizeof(ksi));
            regs->eax = 0;
            break;
        }

        case SYS_GETRLIMIT:
        case __NR_setrlimit:
        case SYS_PRLIMIT64: {
            struct linux_rlimit64 {
                uint64_t rlim_cur;
                uint64_t rlim_max;
            };
            int resource;
            const struct linux_rlimit64* new_rlim = NULL;
            struct linux_rlimit64* old_rlim = NULL;
            if (regs->eax == SYS_PRLIMIT64) {
                resource = (int) regs->ecx;
                new_rlim = (const struct linux_rlimit64*) regs->edx;
                old_rlim = (struct linux_rlimit64*) regs->esi;
            } else if (regs->eax == SYS_GETRLIMIT) {
                resource = (int) regs->ebx;
                old_rlim = (struct linux_rlimit64*) regs->ecx;
            } else {
                resource = (int) regs->ebx;
                new_rlim = (const struct linux_rlimit64*) regs->ecx;
            }
            if (new_rlim) {
                int err = validate_user_buffer(new_rlim, sizeof(struct linux_rlimit64), 0);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
            }
            if (old_rlim) {
                int err = validate_user_buffer(old_rlim, sizeof(struct linux_rlimit64), 1);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                struct linux_rlimit64 kr;
                if (resource == 3) { /* RLIMIT_STACK */
                    kr.rlim_cur = USER_STACK_MAX_GROWTH;
                    kr.rlim_max = USER_STACK_MAX_GROWTH;
                } else if (resource == 7) { /* RLIMIT_NOFILE */
                    kr.rlim_cur = MAX_FDS;
                    kr.rlim_max = MAX_FDS;
                } else {
                    kr.rlim_cur = ~0ULL;
                    kr.rlim_max = ~0ULL;
                }
                copy_to_user(old_rlim, &kr, sizeof(kr));
            }
            regs->eax = 0;
            break;
        }

        case __NR_getrusage: {
            void* usage = (void*) regs->ecx;
            int err = validate_user_buffer(usage, 144, 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            uint8_t kusage[144];
            memset(kusage, 0, sizeof(kusage));
            copy_to_user(usage, kusage, sizeof(kusage));
            regs->eax = 0;
            break;
        }

        case SYS_GETRANDOM: {
            void* ubuf = (void*) regs->ebx;
            uint64_t buflen = regs->ecx;
            if (buflen == 0) { regs->eax = 0; break; }
            if (buflen > 0x7FFFF000ULL) buflen = 0x7FFFF000ULL;
            int err = validate_user_buffer(ubuf, buflen, 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            uint8_t tmp[256];
            uint64_t done = 0;
            while (done < buflen) {
                uint32_t chunk = (uint32_t)((buflen - done > sizeof(tmp)) ? sizeof(tmp) : (buflen - done));
                random_get_bytes(tmp, chunk);
                copy_to_user((uint8_t*)ubuf + done, tmp, chunk);
                done += chunk;
            }
            regs->eax = buflen;
            break;
        }

        case __NR_statfs:
        case __NR_fstatfs: {
            void* ubuf = (void*) regs->ecx;
            if (regs->eax == __NR_statfs) {
                const char* upath = (const char*) regs->ebx;
                int err = validate_user_string(upath);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
            } else {
                int fd = (int) regs->ebx;
                process_t* me = scheduler_current();
                if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                    regs->eax = (uint64_t) -EBADF;
                    break;
                }
            }
            int err = validate_user_buffer(ubuf, 120, 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            uint64_t kstatfs[15];
            memset(kstatfs, 0, sizeof(kstatfs));
            kstatfs[0] = 0x4d44; /* MSDOS_SUPER_MAGIC / TMPFS_MAGIC */
            kstatfs[1] = 4096;   /* f_bsize */
            kstatfs[2] = 16384;  /* f_blocks */
            kstatfs[3] = 8192;   /* f_bfree */
            kstatfs[4] = 8192;   /* f_bavail */
            kstatfs[5] = 4096;   /* f_files */
            kstatfs[6] = 2048;   /* f_ffree */
            kstatfs[8] = 255;    /* f_namelen */
            kstatfs[9] = 4096;   /* f_frsize */
            copy_to_user(ubuf, kstatfs, sizeof(kstatfs));
            regs->eax = 0;
            break;
        }

        case __NR_clock_getres: {
            struct timespec* res = (struct timespec*) regs->ecx;
            if (res) {
                int err = validate_user_buffer(res, sizeof(struct timespec), 1);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                struct timespec kres = { 0, 10000000L }; /* 10ms */
                copy_to_user(res, &kres, sizeof(kres));
            }
            regs->eax = 0;
            break;
        }

        case __NR_clock_nanosleep: {
            int flags = (int) regs->ecx;
            const struct timespec* req = (const struct timespec*) regs->edx;
            struct timespec* rem = (struct timespec*) regs->esi;
            int err = validate_user_buffer(req, sizeof(struct timespec), 0);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            struct timespec kreq;
            copy_from_user(&kreq, req, sizeof(struct timespec));
            if (kreq.tv_nsec < 0 || kreq.tv_nsec >= 1000000000L) {
                regs->eax = (uint64_t) -EINVAL;
                break;
            }
            uint32_t ticks = 0;
            if (flags & 1) { /* TIMER_ABSTIME */
                uint32_t now_ticks = timer_get_ticks();
                int64_t target_ticks = kreq.tv_sec * 100LL + (kreq.tv_nsec + 9999999LL) / 10000000LL;
                if (target_ticks > (int64_t)now_ticks) {
                    ticks = (uint32_t)(target_ticks - now_ticks);
                }
            } else {
                ticks = (uint32_t)(kreq.tv_sec * 100ULL + (kreq.tv_nsec + 9999999ULL) / 10000000ULL);
            }
            if (ticks == 0) { regs->eax = 0; break; }
            process_t* me = scheduler_current();
            uint32_t start = timer_get_ticks();
            uint32_t deadline = start + ticks;
            timer_sleep_enqueue(me, deadline);
            scheduler_wait_channel(&me->sleep_deadline);
            timer_sleep_dequeue(me);
            uint32_t now = timer_get_ticks();
            if (now < deadline) {
                if (rem && !(flags & 1)) {
                    if (validate_user_buffer(rem, sizeof(struct timespec), 1) == 0) {
                        uint32_t rem_ticks = deadline - now;
                        struct timespec krem = { rem_ticks / 100u, (rem_ticks % 100u) * 10000000L };
                        copy_to_user(rem, &krem, sizeof(krem));
                    }
                }
                regs->eax = (uint64_t) -EINTR;
            } else {
                regs->eax = 0;
            }
            break;
        }

        case __NR_rt_sigpending: {
            sigset_t* uset = (sigset_t*) regs->ebx;
            int err = validate_user_buffer(uset, sizeof(sigset_t), 1);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            process_t* me = scheduler_current();
            sigset_t pend = me ? (me->sig_pending & me->sig_blocked) : 0;
            copy_to_user(uset, &pend, sizeof(sigset_t));
            regs->eax = 0;
            break;
        }

        case __NR_rt_sigsuspend: {
            const sigset_t* umask = (const sigset_t*) regs->ebx;
            int err = validate_user_buffer(umask, sizeof(sigset_t), 0);
            if (err != 0) { regs->eax = (uint64_t) err; break; }
            process_t* me = scheduler_current();
            sigset_t old_mask = me->sig_blocked;
            sigset_t new_mask = 0;
            copy_from_user(&new_mask, umask, sizeof(sigset_t));
            new_mask &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));
            me->sig_blocked = new_mask;
            int r = sys_pause();
            me->sig_blocked = old_mask;
            regs->eax = (uint64_t) r;
            break;
        }

        case __NR_sigaltstack: {
            const void* uss = (const void*) regs->ebx;
            void* uoss = (void*) regs->ecx;
            if (uss) {
                int err = validate_user_buffer(uss, 24, 0);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
            }
            if (uoss) {
                int err = validate_user_buffer(uoss, 24, 1);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                uint64_t koss[3] = { 0, 2 /* SS_DISABLE */, 0 };
                copy_to_user(uoss, koss, sizeof(koss));
            }
            regs->eax = 0;
            break;
        }

        case __NR_ppoll: {
            const struct timespec* uts = (const struct timespec*) regs->edx;
            int timeout = -1;
            if (uts) {
                int err = validate_user_buffer(uts, sizeof(struct timespec), 0);
                if (err != 0) { regs->eax = (uint64_t) err; break; }
                struct timespec kts;
                copy_from_user(&kts, uts, sizeof(kts));
                if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L) {
                    regs->eax = (uint64_t) -EINVAL;
                    break;
                }
                timeout = (int)(kts.tv_sec * 1000LL + (kts.tv_nsec + 999999LL) / 1000000LL);
            }
            /* Delegate to poll by rewriting args */
            regs->edx = (uint64_t)(int64_t)timeout;
            goto do_poll_common;
        }

        case __NR_select:
        case __NR_pselect6: {
            int nfds = (int) regs->ebx;
            uint64_t* readfds = (uint64_t*) regs->ecx;
            uint64_t* writefds = (uint64_t*) regs->edx;
            uint64_t* exceptfds = (uint64_t*) regs->esi;
            void* utimeout = (void*) regs->edi;
            if (nfds < 0 || nfds > 1024) { regs->eax = (uint64_t) -EINVAL; break; }
            if (nfds > MAX_FDS) nfds = MAX_FDS;
            int timeout_ms = -1;
            if (utimeout) {
                if (regs->eax == __NR_select) {
                    struct timeval ktv;
                    if (validate_user_buffer(utimeout, sizeof(ktv), 0) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
                    copy_from_user(&ktv, utimeout, sizeof(ktv));
                    timeout_ms = (int)(ktv.tv_sec * 1000LL + (ktv.tv_usec + 999LL) / 1000LL);
                } else {
                    struct timespec kts;
                    if (validate_user_buffer(utimeout, sizeof(kts), 0) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
                    copy_from_user(&kts, utimeout, sizeof(kts));
                    timeout_ms = (int)(kts.tv_sec * 1000LL + (kts.tv_nsec + 999999LL) / 1000000LL);
                }
                if (timeout_ms < 0) timeout_ms = 0;
            }
            uint64_t rmask = 0, wmask = 0, emask = 0;
            if (readfds) {
                if (validate_user_buffer(readfds, sizeof(uint64_t), 1) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
                copy_from_user(&rmask, readfds, sizeof(uint64_t));
            }
            if (writefds) {
                if (validate_user_buffer(writefds, sizeof(uint64_t), 1) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
                copy_from_user(&wmask, writefds, sizeof(uint64_t));
            }
            if (exceptfds) {
                if (validate_user_buffer(exceptfds, sizeof(uint64_t), 1) != 0) { regs->eax = (uint64_t) -EFAULT; break; }
                copy_from_user(&emask, exceptfds, sizeof(uint64_t));
            }
            process_t* me = scheduler_current();
            uint32_t start_ticks = timer_get_ticks();
            uint32_t deadline = (timeout_ms > 0) ? (start_ticks + (uint32_t)((timeout_ms + 9) / 10)) : 0;
            uint64_t out_r = 0, out_w = 0, out_e = 0;
            int total_ready = 0;
            for (;;) {
                if (signal_has_deliverable(me)) {
                    total_ready = -EINTR;
                    break;
                }
                me->num_poll_channels = 0;
                out_r = 0; out_w = 0; out_e = 0;
                total_ready = 0;
                for (int fd = 0; fd < nfds; fd++) {
                    short ev = 0;
                    if (rmask & (1ULL << fd)) ev |= POLLIN;
                    if (wmask & (1ULL << fd)) ev |= POLLOUT;
                    if (emask & (1ULL << fd)) ev |= POLLERR;
                    if (!ev) continue;
                    if (!me->fds[fd]) {
                        total_ready = -EBADF;
                        break;
                    }
                    void* chan = NULL;
                    short rev = vfs_poll(me->fds[fd], ev, &chan);
                    if ((rmask & (1ULL << fd)) && (rev & (POLLIN | POLLHUP | POLLERR))) {
                        out_r |= (1ULL << fd);
                        total_ready++;
                    }
                    if ((wmask & (1ULL << fd)) && (rev & (POLLOUT | POLLERR))) {
                        out_w |= (1ULL << fd);
                        total_ready++;
                    }
                    if ((emask & (1ULL << fd)) && (rev & POLLERR)) {
                        out_e |= (1ULL << fd);
                        total_ready++;
                    }
                    if (rev == 0 && chan && me->num_poll_channels < MAX_POLL_CHANNELS) {
                        me->poll_channels[me->num_poll_channels++] = chan;
                    }
                }
                if (total_ready != 0 || timeout_ms == 0) break;
                if (timeout_ms > 0 && timer_get_ticks() >= deadline) break;
                if (timeout_ms > 0) {
                    timer_sleep_enqueue(me, deadline);
                    if (me->num_poll_channels < MAX_POLL_CHANNELS)
                        me->poll_channels[me->num_poll_channels++] = &me->sleep_deadline;
                }
                scheduler_wait_poll();
                if (timeout_ms > 0) timer_sleep_dequeue(me);
            }
            me->num_poll_channels = 0;
            if (total_ready >= 0) {
                if (readfds) copy_to_user(readfds, &out_r, sizeof(uint64_t));
                if (writefds) copy_to_user(writefds, &out_w, sizeof(uint64_t));
                if (exceptfds) copy_to_user(exceptfds, &out_e, sizeof(uint64_t));
            }
            regs->eax = (uint64_t) total_ready;
            break;
        }

        case SYS_TICKS: {
            regs->eax = timer_get_ticks();
            break;
        }

        case SYS_FREE_FRAMES: {
            regs->eax = pmm_free_frame_count();
            break;
        }

        case SYS_MMAP: {
            uint64_t addr_hint = regs->ebx;
            uint64_t len       = regs->ecx;
            int prot           = (int) regs->edx;
            int flags          = (int) regs->esi;
            int fd             = (int) regs->edi;
            uint64_t offset    = regs->ebp;
            process_t* me = scheduler_current();

            if (len == 0 || (offset & 0xFFFu)) {
                regs->eax = (uint64_t) -EINVAL;
                break;
            }

            len = (len + 4095ULL) & ~4095ULL;

            struct vnode* file = NULL;
            uint64_t file_size = 0;
            int is_device = 0;

            int is_shared = (flags & MAP_SHARED) ? 1 : 0;
            int is_private = (flags & MAP_PRIVATE) ? 1 : 0;
            if ((is_shared && is_private) || (!is_shared && !is_private)) {
                regs->eax = (uint64_t) -EINVAL;
                break;
            }

            if (!(flags & MAP_ANON)) {
                if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                    regs->eax = (uint64_t) -EBADF;
                    break;
                }
                open_file_t* of = me->fds[fd];
                if (!of->vnode) {
                    regs->eax = (uint64_t) -EACCES;
                    break;
                }
                file = of->vnode;
                if (file->ops && file->ops->mmap) {
                    is_device = 1;
                } else if (of->type != OPEN_FILE_VNODE) {
                    regs->eax = (uint64_t) -EACCES;
                    break;
                }

                if (is_shared && !is_device) {
                    if (!tmpfs_is_tmpfs_vnode(file)) {
                        /* MAP_SHARED on a FAT file returns -ENODEV */
                        regs->eax = (uint64_t) -ENODEV;
                        break;
                    }
                    if ((prot & PROT_WRITE) && (of->flags & O_ACCMODE) == O_RDONLY) {
                        regs->eax = (uint64_t) -EACCES;
                        break;
                    }
                }

                if (offset < file->size) {
                    uint64_t rem = file->size - offset;
                    file_size = (len < rem) ? len : rem;
                } else {
                    file_size = 0;
                }
            }

            uint64_t target_addr = 0;
            if (flags & MAP_FIXED) {
                if (!addr_hint || (addr_hint & 0xFFFu) != 0 ||
                    addr_hint < 0x1000 || addr_hint + len > 0x0000800000000000ULL ||
                    addr_hint + len < addr_hint) {
                    regs->eax = (uint64_t) -EINVAL;
                    break;
                }
                vma_unmap_range(me, addr_hint, len);
                target_addr = addr_hint;
            } else if (addr_hint != 0 && (addr_hint & 0xFFFu) == 0 &&
                       addr_hint >= 0x1000 && addr_hint + len <= 0x00007FFFFFFFFFFFULL &&
                       !vma_overlaps(me, addr_hint, addr_hint + len)) {
                target_addr = addr_hint;
            } else {
                target_addr = vma_find_free_gap(me, len);
                if (!target_addr) {
                    regs->eax = (uint64_t) -ENOMEM;
                    break;
                }
            }

            uint32_t vma_flags = (flags & MAP_ANON) ? VMA_FLAG_ANON : VMA_FLAG_FILE;
            if (is_device) {
                vma_flags |= VMA_FLAG_DEVICE;
            }
            if (is_shared) {
                vma_flags |= VMA_FLAG_SHARED;
            }
            uint32_t vma_prot = 0;
            if (prot & PROT_READ)  vma_prot |= VMA_PROT_READ;
            if (prot & PROT_WRITE) vma_prot |= VMA_PROT_WRITE;
            if (prot & PROT_EXEC)  vma_prot |= VMA_PROT_EXEC;

            vma_t* new_vma = vma_create(target_addr, target_addr + len,
                                        vma_prot, vma_flags, file, offset, file_size);
            if (!new_vma) {
                regs->eax = (uint64_t) -ENOMEM;
                break;
            }

            if (vma_insert(&me->vma_list, new_vma) != 0) {
                if (file) vnode_unref(file);
                kfree(new_vma);
                regs->eax = (uint64_t) -ENOMEM;
                break;
            }

            if (is_device && file && file->ops && file->ops->mmap) {
                int mr = file->ops->mmap(file, new_vma);
                if (mr != 0) {
                    vma_unmap_range(me, target_addr, len);
                    regs->eax = (uint64_t) mr;
                    break;
                }
            }

            /* Pre-allocate and map frames for MAP_SHARED | MAP_ANON */
            if ((vma_flags & (VMA_FLAG_ANON | VMA_FLAG_SHARED)) == (VMA_FLAG_ANON | VMA_FLAG_SHARED)) {
                uint32_t pte_flags = PAGE_PRESENT | PAGE_USER;
                if (prot & PROT_WRITE) pte_flags |= PAGE_WRITE;
                int alloc_fail = 0;
                for (uint64_t va = target_addr; va < target_addr + len; va += 4096) {
                    uint64_t fp = pmm_alloc_frame();
                    if (!fp) {
                        alloc_fail = 1;
                        break;
                    }
                    memset(P2V(fp), 0, FRAME_SIZE);
                    if (vmm_map_page(&me->as, va, fp, pte_flags) != 0) {
                        pmm_unref(fp);
                        alloc_fail = 1;
                        break;
                    }
                }
                if (alloc_fail) {
                    vma_unmap_range(me, target_addr, len);
                    regs->eax = (uint64_t) -ENOMEM;
                    break;
                }
            }

            regs->eax = target_addr;
            break;
        }

        case SYS_MUNMAP: {
            uint64_t addr = regs->ebx;
            uint64_t len  = regs->ecx;
            process_t* me = scheduler_current();

            if ((addr & 0xFFFu) || len == 0) {
                regs->eax = (uint64_t) -EINVAL;
                break;
            }
            int r = vma_unmap_range(me, addr, len);
            regs->eax = (uint64_t) r;
            break;
        }

        case SYS_MPROTECT: {
            uint64_t addr = regs->ebx;
            uint64_t len  = regs->ecx;
            int prot      = (int) regs->edx;
            process_t* me = scheduler_current();

            if ((addr & 0xFFFu) || len == 0) {
                regs->eax = (uint64_t) -EINVAL;
                break;
            }
            int r = vma_mprotect(me, addr, len, prot);
            regs->eax = (uint64_t) r;
            break;
        }

        case SYS_IOCTL: {
            int fd = (int) regs->ebx;
            unsigned long req = (unsigned long) regs->ecx;
            void* argp = (void*) regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            if (of->vnode && of->vnode->ops && of->vnode->ops->ioctl) {
                regs->eax = (uint32_t) of->vnode->ops->ioctl(of->vnode, req, argp);
                break;
            }

            if (of->type != OPEN_FILE_CONSOLE || !of->vnode || !devfs_is_tty_vnode(of->vnode)) {
                regs->eax = (uint32_t) -ENOTTY;
                break;
            }

            regs->eax = (uint32_t) tty_ioctl(global_tty, req, argp);
            break;
        }

        case SYS_TCGETATTR: {
            int fd = (int) regs->ebx;
            void* argp = (void*) regs->ecx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            if (of->vnode && of->vnode->ops && of->vnode->ops->ioctl) {
                regs->eax = (uint32_t) of->vnode->ops->ioctl(of->vnode, TCGETS, argp);
                break;
            }
            if (of->type != OPEN_FILE_CONSOLE || !of->vnode || !devfs_is_tty_vnode(of->vnode)) {
                regs->eax = (uint32_t) -ENOTTY;
                break;
            }

            regs->eax = (uint32_t) tty_ioctl(global_tty, TCGETS, argp);
            break;
        }

        case SYS_TCSETATTR: {
            int fd = (int) regs->ebx;
            int opt = (int) regs->ecx;
            void* argp = (void*) regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            unsigned long req = (opt == 0 ? TCSETS : (opt == 1 ? TCSETSW : TCSETSF));
            if (of->vnode && of->vnode->ops && of->vnode->ops->ioctl) {
                regs->eax = (uint32_t) of->vnode->ops->ioctl(of->vnode, req, argp);
                break;
            }
            if (of->type != OPEN_FILE_CONSOLE || !of->vnode || !devfs_is_tty_vnode(of->vnode)) {
                regs->eax = (uint32_t) -ENOTTY;
                break;
            }

            regs->eax = (uint32_t) tty_ioctl(global_tty, req, argp);
            break;
        }

        case SYS_SETPGID: {
            int pid = (int) regs->ebx;
            int pgid = (int) regs->ecx;
            process_t* me = scheduler_current();
            if (!me) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }

            process_t* target = (pid == 0 || pid == me->pid) ? me : process_find_by_pid(pid);
            if (!target) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }

            if (target != me && target->ppid != me->pid) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }

            if (pgid < 0) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            if (pgid == 0) pgid = target->pid;

            target->pgid = pgid;
            regs->eax = 0;
            break;
        }

        case SYS_GETPGID: {
            int pid = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (!me) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }
            process_t* target = (pid == 0) ? me : process_find_by_pid(pid);
            if (!target) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }
            regs->eax = (uint32_t) target->pgid;
            break;
        }

        case SYS_GETPPID: {
            process_t* me = scheduler_current();
            regs->eax = (uint32_t)(me ? me->ppid : 0);
            break;
        }

        case SYS_GETSID: {
            int pid = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (!me) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }
            process_t* target = (pid == 0) ? me : process_find_by_pid(pid);
            if (!target) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }
            regs->eax = (uint32_t) target->sid;
            break;
        }

        case SYS_SETSID: {
            process_t* me = scheduler_current();
            if (!me) {
                regs->eax = (uint32_t) -ESRCH;
                break;
            }
            if (me->pgid == me->pid) {
                regs->eax = (uint32_t) -EPERM;
                break;
            }
            me->sid = me->pid;
            me->pgid = me->pid;
            regs->eax = (uint32_t) me->sid;
            break;
        }

        case SYS_KILL: {
            int pid = (int) regs->ebx;
            int sig = (int) regs->ecx;
            regs->eax = (uint32_t) sys_kill(pid, sig);
            break;
        }

        case SYS_SIGACTION: {
            int sig = (int) regs->ebx;
            const struct sigaction* act = (const struct sigaction*) regs->ecx;
            struct sigaction* oldact = (struct sigaction*) regs->edx;
            regs->eax = (uint32_t) sys_sigaction(sig, act, oldact);
            break;
        }

        case SYS_SIGPROCMASK: {
            int how = (int) regs->ebx;
            const sigset_t* set = (const sigset_t*) regs->ecx;
            sigset_t* oldset = (sigset_t*) regs->edx;
            regs->eax = (uint32_t) sys_sigprocmask(how, set, oldset);
            break;
        }

        case SYS_SIGRETURN: {
            regs->eax = (uint64_t) sys_sigreturn(regs->raw);
            break;
        }

        case SYS_PAUSE: {
            regs->eax = (uint32_t) sys_pause();
            break;
        }

        case SYS_ALARM: {
            unsigned int sec = (unsigned int) regs->ebx;
            regs->eax = (uint32_t) sys_alarm(sec);
            break;
        }

        case SYS_TIME: {
            uint32_t* tloc = (uint32_t*) regs->ebx;
            uint32_t cur = rtc_get_epoch();
            if (tloc) {
                int err = validate_user_buffer(tloc, sizeof(uint32_t), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
                copy_to_user(tloc, &cur, sizeof(uint32_t));
            }
            regs->eax = cur;
            break;
        }

        case SYS_GETTIMEOFDAY: {
            struct timeval* tv = (struct timeval*) regs->ebx;
            struct timezone* tz = (struct timezone*) regs->ecx;
            if (tv) {
                int err = validate_user_buffer(tv, sizeof(struct timeval), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
                uint32_t ticks = timer_get_ticks();
                struct timeval ktv;
                ktv.tv_sec = rtc_get_boot_epoch() + (ticks / 100u);
                ktv.tv_usec = (ticks % 100u) * 10000u;
                copy_to_user(tv, &ktv, sizeof(struct timeval));
            }
            if (tz) {
                int err = validate_user_buffer(tz, sizeof(struct timezone), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
                struct timezone ktz = { 0, 0 };
                copy_to_user(tz, &ktz, sizeof(struct timezone));
            }
            regs->eax = 0;
            break;
        }

        case SYS_CLOCK_GETTIME: {
            int clk_id = (int) regs->ebx;
            struct timespec* tp = (struct timespec*) regs->ecx;
            int err = validate_user_buffer(tp, sizeof(struct timespec), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            uint32_t ticks = timer_get_ticks();
            struct timespec ktp;
            if (clk_id == CLOCK_REALTIME) {
                ktp.tv_sec = rtc_get_boot_epoch() + (ticks / 100u);
                ktp.tv_nsec = (ticks % 100u) * 10000000u;
            } else if (clk_id == CLOCK_MONOTONIC) {
                ktp.tv_sec = ticks / 100u;
                ktp.tv_nsec = (ticks % 100u) * 10000000u;
            } else {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            copy_to_user(tp, &ktp, sizeof(struct timespec));
            regs->eax = 0;
            break;
        }

        case SYS_NANOSLEEP: {
            const struct timespec* req = (const struct timespec*) regs->ebx;
            struct timespec* rem = (struct timespec*) regs->ecx;
            int err = validate_user_buffer(req, sizeof(struct timespec), 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            struct timespec kreq;
            copy_from_user(&kreq, req, sizeof(struct timespec));
            if (kreq.tv_nsec >= 1000000000u) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            uint32_t ticks = kreq.tv_sec * 100u + (kreq.tv_nsec + 9999999u) / 10000000u;
            process_t* me = scheduler_current();
            if (ticks == 0) { regs->eax = 0; break; }
            uint32_t start = timer_get_ticks();
            uint32_t deadline = start + ticks;
            timer_sleep_enqueue(me, deadline);
            scheduler_wait_channel(&me->sleep_deadline);
            timer_sleep_dequeue(me);
            uint32_t now = timer_get_ticks();
            if (now < deadline) {
                if (rem) {
                    err = validate_user_buffer(rem, sizeof(struct timespec), 1);
                    if (err == 0) {
                        uint32_t rem_ticks = deadline - now;
                        struct timespec krem;
                        krem.tv_sec = rem_ticks / 100u;
                        krem.tv_nsec = (rem_ticks % 100u) * 10000000u;
                        copy_to_user(rem, &krem, sizeof(struct timespec));
                    }
                }
                regs->eax = (uint32_t) -EINTR;
            } else {
                regs->eax = 0;
            }
            break;
        }

        case SYS_NICE: {
            int inc = (int) regs->ebx;
            process_t* me = scheduler_current();
            me->nice += inc;
            if (me->nice < -20) me->nice = -20;
            if (me->nice > 19) me->nice = 19;
            regs->eax = (uint32_t) me->nice;
            break;
        }

        case SYS_REBOOT: {
            uint32_t magic1 = regs->ebx;
            uint32_t magic2 = regs->ecx;
            uint32_t cmd = regs->edx;

            if (magic1 != 0xfee1dead ||
                (magic2 != 672274793 && magic2 != 0x28121969 && magic2 != 0x05121996 &&
                 magic2 != 0x16041998 && magic2 != 0x20112000)) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            extern void kernel_reboot(uint32_t cmd);
            kernel_reboot(cmd);
            regs->eax = 0;
            break;
        }

        case SYS_POLL:
        do_poll_common: {
            struct pollfd* user_fds = (struct pollfd*) regs->ebx;
            uint32_t nfds = (uint32_t) regs->ecx;
            int timeout = (int) regs->edx;

            if (nfds > MAX_POLL_CHANNELS) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            if (nfds > 0) {
                if (!user_fds) {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
                int err = validate_user_buffer(user_fds, nfds * sizeof(struct pollfd), 1);
                if (err != 0) {
                    regs->eax = (uint32_t) err;
                    break;
                }
            }

            process_t* me = scheduler_current();

            uint32_t start_ticks = timer_get_ticks();
            uint32_t deadline = 0;
            if (timeout > 0) {
                uint32_t wait_ticks = (uint32_t)((timeout + 9) / 10);
                deadline = start_ticks + wait_ticks;
            }

            struct pollfd kfds[MAX_POLL_CHANNELS];
            if (nfds > 0) {
                if (copy_from_user(kfds, user_fds, nfds * sizeof(struct pollfd)) != 0) {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }

            int ret_val = 0;

            for (;;) {
                if (signal_has_deliverable(me)) {
                    ret_val = -EINTR;
                    break;
                }

                me->num_poll_channels = 0;
                int ready_count = 0;

                for (uint32_t i = 0; i < nfds; i++) {
                    kfds[i].revents = 0;
                    int fd = kfds[i].fd;
                    if (fd < 0) {
                        continue;
                    }
                    if (fd >= MAX_FDS || !me->fds[fd]) {
                        kfds[i].revents = POLLNVAL;
                        ready_count++;
                        continue;
                    }

                    open_file_t* of = me->fds[fd];
                    void* chan = 0;
                    short rev = vfs_poll(of, kfds[i].events, &chan);
                    rev &= (kfds[i].events | POLLERR | POLLHUP | POLLNVAL);
                    kfds[i].revents = rev;
                    if (rev != 0) {
                        ready_count++;
                    } else if (chan && me->num_poll_channels < MAX_POLL_CHANNELS) {
                        int already = 0;
                        for (int c = 0; c < me->num_poll_channels; c++) {
                            if (me->poll_channels[c] == chan) {
                                already = 1;
                                break;
                            }
                        }
                        if (!already) {
                            me->poll_channels[me->num_poll_channels++] = chan;
                        }
                    }
                }

                if (ready_count > 0) {
                    ret_val = ready_count;
                    break;
                }

                if (timeout == 0) {
                    ret_val = 0;
                    break;
                }

                if (timeout > 0) {
                    uint32_t now = timer_get_ticks();
                    if (now >= deadline) {
                        ret_val = 0;
                        break;
                    }
                }

                /* Need to wait */
                if (timeout > 0) {
                    timer_sleep_enqueue(me, deadline);
                    if (me->num_poll_channels < MAX_POLL_CHANNELS) {
                        me->poll_channels[me->num_poll_channels++] = &me->sleep_deadline;
                    }
                }

                scheduler_wait_poll();

                if (timeout > 0) {
                    timer_sleep_dequeue(me);
                }

                /* Check timeout again after waking */
                if (timeout > 0 && timer_get_ticks() >= deadline) {
                    ready_count = 0;
                    for (uint32_t i = 0; i < nfds; i++) {
                        kfds[i].revents = 0;
                        int fd = kfds[i].fd;
                        if (fd < 0) continue;
                        if (fd >= MAX_FDS || !me->fds[fd]) {
                            kfds[i].revents = POLLNVAL;
                            ready_count++;
                            continue;
                        }
                        short rev = vfs_poll(me->fds[fd], kfds[i].events, 0);
                        rev &= (kfds[i].events | POLLERR | POLLHUP | POLLNVAL);
                        kfds[i].revents = rev;
                        if (rev != 0) ready_count++;
                    }
                    ret_val = ready_count;
                    break;
                }
            }

            me->num_poll_channels = 0;

            if (nfds > 0 && ret_val >= 0) {
                if (copy_to_user(user_fds, kfds, nfds * sizeof(struct pollfd)) != 0) {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }

            regs->eax = (uint32_t) ret_val;
            break;
        }

        case SYS_MSYNC: {
            regs->eax = 0;
            break;
        }

        case SYS_SOCKET: {
            int domain = (int) regs->ebx;
            int type = (int) regs->ecx;
            int protocol = (int) regs->edx;

            int flags = type & ~0xF;
            type = type & 0xF;

            if (domain != AF_UNIX && domain != AF_INET) {
                regs->eax = (uint32_t) -EAFNOSUPPORT;
                break;
            }
            if (domain == AF_UNIX && type != SOCK_STREAM) {
                regs->eax = (uint32_t) -EPROTONOSUPPORT;
                break;
            }
            if (domain == AF_INET && type != SOCK_STREAM && type != SOCK_DGRAM) {
                regs->eax = (uint32_t) -EPROTONOSUPPORT;
                break;
            }

            process_t* me = scheduler_current();

            /* Agent socket restriction: default-deny.
             * An agent process (p->agent_id > 0) calling socket(AF_INET, ...)
             * gets -EACCES unless it holds CAP_NET_RAW_INET (strictly audited).
             */
            if (domain == AF_INET && me && me->agent_id > 0) {
                agent_t* ag = agent_get_by_id(me->agent_id);
                if (!ag || !(ag->caps.mask & CAP_NET_RAW_INET)) {
                    agent_audit_log(me->agent_id, AUDIT_EVENT_NET_DENIED, "socket_af_inet_denied");
                    regs->eax = (uint32_t) -EACCES;
                    break;
                }
            }

            int slot = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!me->fds[i]) { slot = i; break; }
            }
            if (slot < 0) {
                regs->eax = (uint32_t) -EMFILE;
                break;
            }

            socket_t* s = socket_alloc(domain, type, protocol);
            if (!s) {
                regs->eax = (uint32_t) -ENOMEM;
                break;
            }

            int of_flags = O_RDWR;
            if (flags & SOCK_NONBLOCK) of_flags |= O_NONBLOCK;
            open_file_t* of = open_file_alloc(OPEN_FILE_SOCKET, NULL, of_flags);
            if (!of) {
                socket_unref(s);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }
            of->sock = s;

            me->fds[slot] = of;
            me->fd_flags[slot] = (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;
            regs->eax = (uint32_t) slot;
            break;
        }

        case SYS_BIND: {
            int fd = (int) regs->ebx;
            const struct sockaddr* uaddr = (const struct sockaddr*) regs->ecx;
            uint32_t addrlen = (uint32_t) regs->edx;

            if (addrlen > sizeof(struct sockaddr_un) + 128) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            int err = validate_user_buffer(uaddr, addrlen, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (me->agent_id > 0 && uaddr) {
                const struct sockaddr_un* un = (const struct sockaddr_un*) uaddr;
                if (un->sun_family == 1) { /* AF_UNIX */
                    if (kstrstr(un->sun_path, "approval.sock") ||
                        kstrstr(un->sun_path, "intent.sock") ||
                        kstrstr(un->sun_path, "display.sock")) {
                        agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "bind to system socket denied");
                        regs->eax = (uint32_t) -EACCES;
                        break;
                    }
                    int aerr = agent_check_access(me, CAP_FS_WRITE, un->sun_path, "bind");
                    if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
                } else if (un->sun_family == 2) { /* AF_INET */
                    agent_t* ag = agent_get_by_id(me->agent_id);
                    if (!ag || (!(ag->caps.mask & CAP_NET_RAW_INET))) {
                        agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "AF_INET bind denied: missing CAP_NET_RAW_INET");
                        regs->eax = (uint32_t) -EACCES;
                        break;
                    }
                }
            }

            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_bind((socket_t*) of->sock, uaddr, addrlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_LISTEN: {
            int fd = (int) regs->ebx;
            int backlog = (int) regs->ecx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_listen((socket_t*) of->sock, backlog);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_ACCEPT:
        case __NR_accept4: {
            int fd = (int) regs->ebx;
            struct sockaddr* uaddr = (struct sockaddr*) regs->ecx;
            uint32_t* uaddrlen = (uint32_t*) regs->edx;
            int flags = (regs->eax == __NR_accept4) ? (int) regs->esi : 0;

            if (uaddrlen) {
                int err = validate_user_buffer(uaddrlen, sizeof(uint32_t), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
                if (*uaddrlen > 0 && uaddr) {
                    err = validate_user_buffer(uaddr, *uaddrlen, 1);
                    if (err != 0) { regs->eax = (uint32_t) err; break; }
                }
            }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }
            if (me) me->current_op_flags = of->flags;
            if (of->flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) {
                flags |= SOCK_NONBLOCK;
            }

            int slot = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!me->fds[i]) { slot = i; break; }
            }
            if (slot < 0) {
                regs->eax = (uint32_t) -EMFILE;
                break;
            }

            socket_t* new_sock = NULL;
            int ret = socket_accept((socket_t*) of->sock, uaddr, uaddrlen, flags, &new_sock);
            if (ret != 0 || !new_sock) {
                regs->eax = (uint32_t) ret;
                break;
            }

            int of_flags = O_RDWR;
            if (flags & SOCK_NONBLOCK) of_flags |= O_NONBLOCK;
            open_file_t* new_of = open_file_alloc(OPEN_FILE_SOCKET, NULL, of_flags);
            if (!new_of) {
                socket_unref(new_sock);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }
            new_of->sock = new_sock;

            me->fds[slot] = new_of;
            me->fd_flags[slot] = (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;
            regs->eax = (uint32_t) slot;
            break;
        }

        case SYS_CONNECT: {
            int fd = (int) regs->ebx;
            const struct sockaddr* uaddr = (const struct sockaddr*) regs->ecx;
            uint32_t addrlen = (uint32_t) regs->edx;

            if (addrlen > sizeof(struct sockaddr_un) + 128) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }
            int err = validate_user_buffer(uaddr, addrlen, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (me->agent_id > 0 && uaddr) {
                const struct sockaddr_un* un = (const struct sockaddr_un*) uaddr;
                if (un->sun_family == 1) { /* AF_UNIX */
                    if (kstrstr(un->sun_path, "approval.sock")) {
                        agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "connect to approval.sock denied");
                        regs->eax = (uint32_t) -EACCES;
                        break;
                    }
                    if (kstrstr(un->sun_path, "intent.sock")) {
                        agent_t* ag = agent_get_by_id(me->agent_id);
                        if (!ag || (!(ag->caps.mask & (CAP_BUS_CALL | CAP_BUS_PROVIDE)))) {
                            agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "connect to intent.sock denied: missing CAP_BUS_*");
                            regs->eax = (uint32_t) -EACCES;
                            break;
                        }
                    }
                    if (kstrstr(un->sun_path, "display.sock")) {
                        agent_t* ag = agent_get_by_id(me->agent_id);
                        if (!ag || (!(ag->caps.mask & (CAP_WINDOW | CAP_WINDOW_OBSERVE | CAP_WINDOW_ACT)))) {
                            agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "connect to display.sock denied: missing CAP_WINDOW_*");
                            regs->eax = (uint32_t) -EACCES;
                            break;
                        }
                    }
                    int aerr = agent_check_access(me, CAP_SOCKET_CONNECT, un->sun_path, "connect");
                    if (aerr < 0) { regs->eax = (uint32_t) aerr; break; }
                } else if (un->sun_family == 2) { /* AF_INET */
                    agent_t* ag = agent_get_by_id(me->agent_id);
                    if (!ag || (!(ag->caps.mask & (CAP_NET_RAW_INET | CAP_NET_HTTP)))) {
                        agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, "AF_INET connect denied: missing CAP_NET_*");
                        regs->eax = (uint32_t) -EACCES;
                        break;
                    }
                }
            }

            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }
            if (me) me->current_op_flags = of->flags;

            int ret = socket_connect((socket_t*) of->sock, uaddr, addrlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_SHUTDOWN: {
            int fd = (int) regs->ebx;
            int how = (int) regs->ecx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_shutdown((socket_t*) of->sock, how);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_GETSOCKNAME: {
            int fd = (int) regs->ebx;
            struct sockaddr* uaddr = (struct sockaddr*) regs->ecx;
            uint32_t* uaddrlen = (uint32_t*) regs->edx;

            if (!uaddr || !uaddrlen) {
                regs->eax = (uint32_t) -EFAULT;
                break;
            }
            int err = validate_user_buffer(uaddrlen, sizeof(uint32_t), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = validate_user_buffer(uaddr, *uaddrlen, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_getsockname((socket_t*) of->sock, uaddr, uaddrlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_GETPEERNAME: {
            int fd = (int) regs->ebx;
            struct sockaddr* uaddr = (struct sockaddr*) regs->ecx;
            uint32_t* uaddrlen = (uint32_t*) regs->edx;

            if (!uaddr || !uaddrlen) {
                regs->eax = (uint32_t) -EFAULT;
                break;
            }
            int err = validate_user_buffer(uaddrlen, sizeof(uint32_t), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = validate_user_buffer(uaddr, *uaddrlen, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_getpeername((socket_t*) of->sock, uaddr, uaddrlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_SOCKETPAIR: {
            int domain = (int) regs->ebx;
            int type = (int) regs->ecx;
            int protocol = (int) regs->edx;
            int* usv = (int*) regs->esi;

            int err = validate_user_buffer(usv, 2 * sizeof(int), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            int flags = type & ~0xF;
            type = type & 0xF;

            if (domain != AF_UNIX) {
                regs->eax = (uint32_t) -EAFNOSUPPORT;
                break;
            }
            if (type != SOCK_STREAM) {
                regs->eax = (uint32_t) -EPROTONOSUPPORT;
                break;
            }

            process_t* me = scheduler_current();
            int fd0 = -1, fd1 = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!me->fds[i]) {
                    if (fd0 < 0) fd0 = i;
                    else if (fd1 < 0) { fd1 = i; break; }
                }
            }
            if (fd0 < 0 || fd1 < 0) {
                regs->eax = (uint32_t) -EMFILE;
                break;
            }

            socket_t* s0 = NULL;
            socket_t* s1 = NULL;
            err = socket_pair(domain, type, protocol, &s0, &s1);
            if (err != 0 || !s0 || !s1) {
                regs->eax = (uint32_t) err;
                break;
            }

            int of_flags = O_RDWR;
            if (flags & SOCK_NONBLOCK) of_flags |= O_NONBLOCK;
            open_file_t* of0 = open_file_alloc(OPEN_FILE_SOCKET, NULL, of_flags);
            open_file_t* of1 = open_file_alloc(OPEN_FILE_SOCKET, NULL, of_flags);
            if (!of0 || !of1) {
                if (of0) open_file_unref(of0);
                if (of1) open_file_unref(of1);
                socket_unref(s0);
                socket_unref(s1);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }
            of0->sock = s0;
            of1->sock = s1;

            me->fds[fd0] = of0;
            me->fd_flags[fd0] = (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;
            me->fds[fd1] = of1;
            me->fd_flags[fd1] = (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0;

            usv[0] = fd0;
            usv[1] = fd1;
            regs->eax = 0;
            break;
        }

        case SYS_SENDTO: {
            int fd = (int) regs->ebx;
            const void* buf = (const void*) regs->ecx;
            uint32_t len = regs->edx;
            int flags = (int) regs->esi;
            const struct sockaddr* dest_addr = (const struct sockaddr*) regs->edi;
            uint32_t addrlen = (uint32_t) regs->ebp;

            int err = validate_user_buffer(buf, len, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            if (dest_addr && addrlen > 0) {
                err = validate_user_buffer(dest_addr, addrlen, 0);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
            }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }
            if (me) me->current_op_flags = of->flags;

            int n = socket_sendto((socket_t*) of->sock, buf, len, flags, dest_addr, addrlen);
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_RECVFROM: {
            int fd = (int) regs->ebx;
            void* buf = (void*) regs->ecx;
            uint32_t len = regs->edx;
            int flags = (int) regs->esi;
            struct sockaddr* src_addr = (struct sockaddr*) regs->edi;
            uint32_t* u_addrlen = (uint32_t*) regs->ebp;

            int err = validate_user_buffer(buf, len, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            uint32_t addrlen = 0;
            if (src_addr && u_addrlen) {
                err = validate_user_buffer(u_addrlen, sizeof(uint32_t), 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
                addrlen = *u_addrlen;
                err = validate_user_buffer(src_addr, addrlen, 1);
                if (err != 0) { regs->eax = (uint32_t) err; break; }
            }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }
            if (me) me->current_op_flags = of->flags;

            int n = socket_recvfrom((socket_t*) of->sock, buf, len, flags, src_addr, u_addrlen ? &addrlen : NULL);
            if (n >= 0 && u_addrlen) {
                *u_addrlen = addrlen;
            }
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_SETSOCKOPT: {
            int fd = (int) regs->ebx;
            int level = (int) regs->ecx;
            int optname = (int) regs->edx;
            const void* optval = (const void*) regs->esi;
            uint32_t optlen = (uint32_t) regs->edi;

            int err = validate_user_buffer(optval, optlen, 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_setsockopt((socket_t*) of->sock, level, optname, optval, optlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_GETSOCKOPT: {
            int fd = (int) regs->ebx;
            int level = (int) regs->ecx;
            int optname = (int) regs->edx;
            void* optval = (void*) regs->esi;
            uint32_t* u_optlen = (uint32_t*) regs->edi;

            int err = validate_user_buffer(u_optlen, sizeof(uint32_t), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = validate_user_buffer(optval, *u_optlen, 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }
            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int ret = socket_getsockopt((socket_t*) of->sock, level, optname, optval, u_optlen);
            regs->eax = (uint32_t) ret;
            break;
        }

        case 46: /* SYS_SENDMSG */
        case 47: /* SYS_RECVMSG */ {
            /* SCM_RIGHTS / ancillary data passing is unsupported; return -EOPNOTSUPP */
            regs->eax = (uint32_t) -EOPNOTSUPP;
            break;
        }

        case 86:  /* SYS_LINK */
        case 265: /* SYS_LINKAT */ {
            /* Hard links are not supported on FAT16 */
            regs->eax = (uint32_t) -EPERM;
            break;
        }

        case SYS_AGENT_CREATE: {
            const char* uname = (const char*) regs->ebx;
            const void* ucaps = (const void*) regs->ecx;
            const agent_budget_t* ubudget = (const agent_budget_t*) regs->edx;

            int err = validate_user_string(uname);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            uint32_t caps_len = sizeof(agent_caps_t);
            if (validate_user_buffer(ucaps, caps_len, 0) != 0) {
                if (validate_user_buffer(ucaps, sizeof(agent_caps_v4_t), 0) == 0) {
                    caps_len = sizeof(agent_caps_v4_t);
                } else if (validate_user_buffer(ucaps, sizeof(agent_caps_v3_t), 0) == 0) {
                    caps_len = sizeof(agent_caps_v3_t);
                } else {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }
            err = validate_user_buffer(ubudget, sizeof(agent_budget_t), 0);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char kname[AGENT_NAME_MAX];
            int i = 0;
            while (uname[i] && i < AGENT_NAME_MAX - 1) {
                kname[i] = uname[i];
                i++;
            }
            kname[i] = '\0';

            static agent_caps_t s_kcaps;
            memset(&s_kcaps, 0, sizeof(s_kcaps));
            const agent_caps_v4_t* v4 = (const agent_caps_v4_t*) ucaps;
            uint32_t off4 = *(const uint32_t*)((const char*)ucaps + 4);
            int is_v4 = 0;
            if (caps_len == sizeof(agent_caps_v4_t)) {
                is_v4 = 1;
            } else if ((v4->mask & (CAP_MEM_READ | CAP_MEM_WRITE)) == 0) {
                if (off4 >= 1 && off4 <= 8) {
                    is_v4 = 1;
                } else if (off4 == 0) {
                    if ((v4->num_fs_write >= 1 && v4->num_fs_write <= 8) ||
                        (v4->num_exec >= 1 && v4->num_exec <= 8) ||
                        (v4->num_connect >= 1 && v4->num_connect <= 8) ||
                        (v4->num_bus_call >= 1 && v4->num_bus_call <= 8) ||
                        (v4->num_bus_provide >= 1 && v4->num_bus_provide <= 8) ||
                        (v4->num_window_observe >= 1 && v4->num_window_observe <= 8) ||
                        (v4->num_window_act >= 1 && v4->num_window_act <= 8)) {
                        is_v4 = 1;
                    }
                }
            }
            if (is_v4) {
                s_kcaps.mask = v4->mask;
                s_kcaps.principal[0] = '\0';
                memcpy(&s_kcaps.num_fs_read, &v4->num_fs_read, sizeof(agent_caps_v4_t) - 4);
            } else if (caps_len == sizeof(agent_caps_v3_t)) {
                const agent_caps_v3_t* v3 = (const agent_caps_v3_t*) ucaps;
                s_kcaps.mask = v3->mask;
                s_kcaps.principal[0] = '\0';
                memcpy(&s_kcaps.num_fs_read, &v3->num_fs_read, 3096);
            } else {
                memcpy(&s_kcaps, ucaps, sizeof(agent_caps_t));
            }

            static agent_budget_t s_kbudget;
            memcpy(&s_kbudget, ubudget, sizeof(agent_budget_t));

            process_t* me = scheduler_current();
            int ret = agent_create_kernel(kname, &s_kcaps, &s_kbudget, me->pid, me->agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_ATTACH: {
            int agent_id = (int) regs->ebx;
            process_t* me = scheduler_current();
            int ret = agent_attach_proc(agent_id, me);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_KILL: {
            int agent_id = (int) regs->ebx;
            int ret = agent_kill_kernel(agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_SUSPEND: {
            int agent_id = (int) regs->ebx;
            int ret = agent_suspend_kernel(agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_RESUME: {
            int agent_id = (int) regs->ebx;
            int ret = agent_resume_kernel(agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_INFO: {
            int agent_id = (int) regs->ebx;
            void* uinfo = (void*) regs->ecx;

            int version = 5;
            if (validate_user_buffer(uinfo, sizeof(agent_info_t), 1) != 0) {
                if (validate_user_buffer(uinfo, sizeof(agent_info_v4_t), 1) == 0) {
                    version = 4;
                } else if (validate_user_buffer(uinfo, sizeof(agent_info_v3_t), 1) == 0) {
                    version = 3;
                } else {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }

            static agent_info_t s_kinfo;
            int ret = agent_get_info(agent_id, &s_kinfo);
            if (ret == 0) {
                if (version == 5) {
                    memcpy(uinfo, &s_kinfo, sizeof(agent_info_t));
                } else if (version == 4) {
                    static agent_info_v4_t s_v4;
                    s_v4.id = s_kinfo.id;
                    memcpy(s_v4.name, s_kinfo.name, AGENT_NAME_MAX);
                    s_v4.owner_pid = s_kinfo.owner_pid;
                    s_v4.parent_agent_id = s_kinfo.parent_agent_id;
                    s_v4.state = s_kinfo.state;
                    s_v4.caps.mask = s_kinfo.caps.mask;
                    memcpy(&s_v4.caps.num_fs_read, &s_kinfo.caps.num_fs_read, 4132);
                    s_v4.budget = s_kinfo.budget;
                    s_v4.usage = s_kinfo.usage;
                    memcpy(uinfo, &s_v4, sizeof(s_v4));
                } else if (version == 3) {
                    static agent_info_v3_t s_v3;
                    s_v3.id = s_kinfo.id;
                    memcpy(s_v3.name, s_kinfo.name, AGENT_NAME_MAX);
                    s_v3.owner_pid = s_kinfo.owner_pid;
                    s_v3.parent_agent_id = s_kinfo.parent_agent_id;
                    s_v3.state = s_kinfo.state;
                    s_v3.caps.mask = s_kinfo.caps.mask;
                    memcpy(&s_v3.caps.num_fs_read, &s_kinfo.caps.num_fs_read, 3096);
                    s_v3.budget = s_kinfo.budget;
                    s_v3.usage = s_kinfo.usage;
                    memcpy(uinfo, &s_v3, sizeof(s_v3));
                }
            }
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_SOCK_PEERINFO: {
            int fd = (int) regs->ebx;
            void* upinfo = (void*) regs->ecx;

            int write_len = sizeof(peerinfo_t);
            if (validate_user_buffer(upinfo, write_len, 1) != 0) {
                if (validate_user_buffer(upinfo, sizeof(int) * 2, 1) == 0) {
                    write_len = sizeof(int) * 2;
                } else {
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            if (of->type != OPEN_FILE_SOCKET || !of->sock) {
                regs->eax = (uint32_t) -ENOTSOCK;
                break;
            }

            int pid_val = 0, agent_val = 0;
            char princ_val[32];
            princ_val[0] = '\0';
            int ret = socket_get_peerinfo(of->sock, &pid_val, &agent_val, princ_val);
            if (ret == 0) {
                peerinfo_t kinfo;
                kinfo.pid = pid_val;
                kinfo.agent_id = agent_val;
                for (size_t i = 0; i < sizeof(kinfo.principal); i++) {
                    kinfo.principal[i] = princ_val[i];
                    if (princ_val[i] == '\0') break;
                }
                kinfo.principal[sizeof(kinfo.principal) - 1] = '\0';
                memcpy(upinfo, &kinfo, write_len);
            }
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_BUS_DAEMON_REGISTER: {
            process_t* me = scheduler_current();
            int ret = agent_bus_daemon_register(me);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AUDIT_APPEND: {
            int agent_id = (int) regs->ebx;
            uint32_t event_type = (uint32_t) regs->ecx;
            const char* udetail = (const char*) regs->edx;

            int err = validate_user_string(udetail);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            char kdetail[64];
            int i = 0;
            while (udetail[i] && i < (int)(sizeof(kdetail) - 1)) {
                kdetail[i] = udetail[i];
                i++;
            }
            kdetail[i] = '\0';

            int ret = agent_audit_append(agent_id, event_type, kdetail);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_CHARGE_TOKENS: {
            int agent_id = (int) regs->ebx;
            uint32_t tokens = (uint32_t) regs->ecx;
            int ret = agent_charge_tokens(agent_id, tokens);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_TOKENS_LEFT: {
            int agent_id = (int) regs->ebx;
            int ret = agent_tokens_left(agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_MODEL_DAEMON_REGISTER: {
            process_t* me = scheduler_current();
            int ret = agent_model_daemon_register(me);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_KILLSWITCH_STATE: {
            int ret;
            if (regs->ebx == 1) {
                ret = agent_killswitch_toggle();
            } else {
                ret = agent_killswitch_active();
            }
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_CHARGE_NET: {
            int agent_id = (int) regs->ebx;
            uint32_t bytes = (uint32_t) regs->ecx;
            int ret = agent_charge_net(agent_id, bytes);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_AGENT_NET_LEFT: {
            int agent_id = (int) regs->ebx;
            int ret = agent_net_left(agent_id);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_NET_DAEMON_REGISTER: {
            process_t* me = scheduler_current();
            int ret = agent_net_daemon_register(me);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_TEST_HARDENING: {
            int type = (int) regs->ebx;
            void* ptr = (void*) regs->ecx;
            if (type == 1) {
                regs->eax = (uint64_t) test_smap_behavior(ptr);
            } else if (type == 2) {
                regs->eax = (uint64_t) test_smep_behavior(ptr);
            } else {
                regs->eax = (uint64_t) -EINVAL;
            }
            break;
        }

        case 1099: {
            /* Debug syscall: test kernel stack overflow & guard page fault */
            serial_printf("[kstack-test] Invoking stack overflow recursion on PID %d...\n",
                          me_syscall_proc ? me_syscall_proc->pid : -1);
            trigger_stack_overflow(1);
            regs->eax = 0;
            break;
        }

        default: {
            process_t* me = scheduler_current();
            uint64_t nr = regs->eax;
            int should_log = 1;
            if (me) {
                uint64_t bit = nr & 511ULL;
                uint64_t word = bit >> 6;
                uint64_t mask = 1ULL << (bit & 63ULL);
                if (me->logged_unimpl_syscalls[word] & mask) {
                    should_log = 0;
                } else {
                    me->logged_unimpl_syscalls[word] |= mask;
                }
                if (me->agent_id > 0) {
                    char detail[48];
                    const char* pfx = "unhandled_syscall_";
                    int di = 0;
                    while (pfx[di]) { detail[di] = pfx[di]; di++; }
                    uint64_t tmp_nr = nr;
                    char rev[24];
                    int ri = 0;
                    if (tmp_nr == 0) rev[ri++] = '0';
                    while (tmp_nr > 0 && ri < 20) {
                        rev[ri++] = (char)('0' + (tmp_nr % 10));
                        tmp_nr /= 10;
                    }
                    while (ri > 0 && di < 46) {
                        detail[di++] = rev[--ri];
                    }
                    detail[di] = '\0';
                    agent_audit_log(me->agent_id, AUDIT_EVENT_DENIED, detail);
                }
            }
            if (should_log) {
                serial_printf("[unimpl-syscall] pid=%d (%s) nr=%lu args=(0x%lx, 0x%lx, 0x%lx) returning -ENOSYS\n",
                              me ? me->pid : 0, me ? me->name : "kernel",
                              nr, regs->ebx, regs->ecx, regs->edx);
            }
            regs->eax = (uint64_t) -ENOSYS;
            break;
        }
    }

    if (me_syscall_proc) me_syscall_proc->current_syscall = 0;

    agent_killswitch_check_deferred();

    /* Lock-sanity assertions: panic if a mutex is held at syscall return
       or if interrupts are disabled on return to user mode */
    kmutex_assert_none_held();
    if ((regs->cs & 3) == 3 && (regs->rflags & 0x200) == 0) {
        serial_printf("\n*** KERNEL PANIC: interrupts disabled in user mode return frame (rflags=0x%016lx)! ***\n", regs->rflags);
        terminal_writestring("\n*** KERNEL PANIC: interrupts disabled in user mode return frame! ***\n");
        asm volatile ("cli");
        for (;;) asm volatile ("hlt");
    }
}

void syscall_dispatch(struct registers* raw_regs) {
    agent_killswitch_check_deferred();

    struct syscall_args sa;
    sa.raw = raw_regs;
    sa.eax = raw_regs->rax;
    sa.cs = raw_regs->cs;
    sa.rflags = raw_regs->rflags;

    if (raw_regs->int_no == 128) {
        /* Legacy 32-bit int 0x80 register layout */
        sa.ebx = raw_regs->rbx;
        sa.ecx = raw_regs->rcx;
        sa.edx = raw_regs->rdx;
        sa.esi = raw_regs->rsi;
        sa.edi = raw_regs->rdi;
        sa.ebp = raw_regs->rbp;
    } else {
        /* Standard 64-bit AMD64 syscall register layout:
           arg1: %rdi, arg2: %rsi, arg3: %rdx, arg4: %r10, arg5: %r8, arg6: %r9 */
        sa.ebx = raw_regs->rdi;
        sa.ecx = raw_regs->rsi;
        sa.edx = raw_regs->rdx;
        sa.esi = raw_regs->r10;
        sa.edi = raw_regs->r8;
        sa.ebp = raw_regs->r9;
    }

    user_access_begin();
    syscall_handler_internal(&sa);
    user_access_end();

    /* In 64-bit, sign-extend 32-bit negative return values (-1 .. -4095) */
    if ((int32_t)sa.eax < 0 && (int32_t)sa.eax >= -4095) {
        raw_regs->rax = (int64_t)(int32_t)sa.eax;
    } else {
        raw_regs->rax = sa.eax;
    }

    /* Check deliverable signals before returning to user mode */
    process_t* me = scheduler_current();
    if (me && !me->is_kernel_task && (raw_regs->cs & 3) == 3) {
        if (signal_has_deliverable(me)) {
            signal_handle_pending(raw_regs);
        }
    }
}

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = (val >> 32) & 0xFFFFFFFF;
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

extern void syscall_entry(void);

void syscall_install(void) {
#ifdef MYOS_LEGACY_INT80
    /* 1. Optional legacy vector 128 (int $0x80) in 64-bit IDT */
    idt_set_gate(128, (uint64_t) isr128, 0x08, 0xEE);
    register_interrupt_handler(128, &syscall_dispatch);
#endif

    /* Configure MSRs for the fast 64-bit `syscall` / `sysret` instruction:
       Enable SCE (System Call Extensions) in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1ULL);

    /* STAR MSR:
       Bits 32..47: Kernel CS (0x08) -> SS is 0x10 (0x08 + 8)
       Bits 48..63: User CS / SS base (0x18) -> SS is 0x23, CS is 0x2B
    */
    wrmsr(MSR_STAR, ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32));

    /* LSTAR MSR: Target entry point for 64-bit `syscall` */
    wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);

    /* FMASK MSR: RFLAGS mask to clear on syscall entry (clear IF, TF, DF) */
    wrmsr(MSR_FMASK, 0x3F7FD5ULL);
}

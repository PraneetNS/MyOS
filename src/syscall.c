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

#define SYS_EXIT       0
#define SYS_WRITE      1
#define SYS_GETPID     2
#define SYS_OPEN       3
#define SYS_READ       4
#define SYS_CLOSE      5
#define SYS_FORK       7
#define SYS_PIPE       8
#define SYS_EXEC       10
#define SYS_WAITPID    11
#define SYS_SBRK       12
#define SYS_SYNC       13
#define SYS_LSEEK      14
#define SYS_STAT       15
#define SYS_FSTAT      16
#define SYS_GETDENTS   17
#define SYS_MKDIR      18
#define SYS_RMDIR      19
#define SYS_UNLINK     20
#define SYS_RENAME     21
#define SYS_CHDIR      22
#define SYS_GETCWD     23
#define SYS_DUP        24
#define SYS_DUP2       25
#define SYS_FCNTL      26
#define SYS_SLEEP      27
#define SYS_TICKS      28

#define WNOHANG        1

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4

extern void isr128(void); /* defined in isr.s */

static int validate_user_buffer(const void* ptr, uint32_t len, int write) {
    if (len == 0) return 0;
    if (!ptr) return -EFAULT;

    process_t* me = scheduler_current();
    if (!me || !me->as.directory) return 0;

    uint32_t start = (uint32_t) ptr;
    uint32_t end = start + len;
    if (end < start) return -EFAULT;

    if (start < 0x400000 || end > 0xC0000000) return -EFAULT;

    uint32_t* dir = me->as.directory;
    uint32_t page_start = start & ~0xFFFu;
    uint32_t page_end = (end - 1) & ~0xFFFu;

    for (uint32_t va = page_start;; va += 4096) {
        uint32_t dir_idx = va >> 22;
        uint32_t tbl_idx = (va >> 12) & 0x3FF;

        if (!(dir[dir_idx] & PAGE_PRESENT) || !(dir[dir_idx] & PAGE_USER)) return -EFAULT;
        uint32_t* tbl = (uint32_t*) (dir[dir_idx] & ~0xFFFu);
        if (!(tbl[tbl_idx] & PAGE_PRESENT) || !(tbl[tbl_idx] & PAGE_USER)) return -EFAULT;
        if (write && !(tbl[tbl_idx] & PAGE_WRITE)) return -EFAULT;

        if (va >= page_end) break;
    }

    return 0;
}

static int validate_user_string(const char* s) {
    if (!s) return -EFAULT;

    process_t* me = scheduler_current();
    if (!me || !me->as.directory) return 0;

    uint32_t va = (uint32_t) s;
    if (va < 0x400000 || va >= 0xC0000000) return -EFAULT;

    uint32_t* dir = me->as.directory;
    int len = 0;

    while (1) {
        if (va < 0x400000 || va >= 0xC0000000) return -EFAULT;
        uint32_t dir_idx = va >> 22;
        uint32_t tbl_idx = (va >> 12) & 0x3FF;

        if (!(dir[dir_idx] & PAGE_PRESENT) || !(dir[dir_idx] & PAGE_USER)) return -EFAULT;
        uint32_t* tbl = (uint32_t*) (dir[dir_idx] & ~0xFFFu);
        if (!(tbl[tbl_idx] & PAGE_PRESENT) || !(tbl[tbl_idx] & PAGE_USER)) return -EFAULT;

        uint32_t page_limit = (va & ~0xFFFu) + 4096;
        while (va < page_limit) {
            char c = *(const char*) va;
            if (c == '\0') return 0;
            len++;
            if (len > 1024) return -ENAMETOOLONG;
            va++;
            if (va >= 0xC0000000) return -EFAULT;
        }
    }
}



static void syscall_handler(struct registers* regs) {
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

        case SYS_EXIT: {
            int status = (int) regs->ebx;
            kprintf("[ok] process exited\n");
            scheduler_exit_current(status); /* never returns */
            break;
        }

        case SYS_GETPID:
            regs->eax = (uint32_t) scheduler_current()->pid;
            break;

        case SYS_OPEN: {
            const char* path = (const char*) regs->ebx;
            int flags = (int) regs->ecx;
            int mode = (int) regs->edx;
            (void) mode;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            int slot = -1;
            for (int i = 3; i < MAX_FDS; i++) {
                if (!me->fds[i]) { slot = i; break; }
            }
            if (slot < 0) { regs->eax = (uint32_t) -EMFILE; break; }

            vnode_t* vn = 0;
            int ret = vfs_resolve_path(path, me->cwd, &vn);
            if (ret == 0 && vn) {
                if (vn->type == VNODE_DIR && ((flags & O_WRONLY) || (flags & O_RDWR))) {
                    vnode_unref(vn);
                    regs->eax = (uint32_t) -EISDIR;
                    break;
                }
                if (flags & O_TRUNC) {
                    if (vn->ops && vn->ops->truncate) {
                        vn->ops->truncate(vn, 0);
                        vn->size = 0;
                    }
                }
            } else {
                if (!(flags & O_CREAT)) {
                    regs->eax = (uint32_t) -ENOENT;
                    break;
                }
                /* Create file */
                vnode_t* parent = 0;
                char child_name[64];
                ret = vfs_resolve_parent(path, me->cwd, &parent, child_name, sizeof(child_name));
                if (ret != 0 || !parent) {
                    regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                    break;
                }
                if (!parent->ops || !parent->ops->create) {
                    vnode_unref(parent);
                    regs->eax = (uint32_t) -ENOSYS;
                    break;
                }
                ret = parent->ops->create(parent, child_name, &vn);
                vnode_unref(parent);
                if (ret != 0 || !vn) {
                    regs->eax = (uint32_t) ret;
                    break;
                }
            }

            open_file_t* of = open_file_alloc(OPEN_FILE_VNODE, vn, flags);
            if (!of) {
                vnode_unref(vn);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }
            if (flags & O_APPEND) {
                of->offset = vn->size;
            }

            me->fds[slot] = of;
            me->fd_flags[slot] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            regs->eax = (uint32_t) slot;
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



        case SYS_FORK: {
            process_t* me = scheduler_current();
            process_t* child = process_fork(me, regs);
            regs->eax = child ? (uint32_t) child->pid : (uint32_t)-1;
            break;
        }

        case SYS_PIPE: {
            int* user_fds = (int*) regs->ebx;
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

            open_file_t* of_read = open_file_alloc(OPEN_FILE_PIPE, 0, O_RDONLY);
            if (!of_read) {
                pipe_close_end(p, O_RDONLY);
                pipe_close_end(p, O_WRONLY);
                regs->eax = (uint32_t) -ENFILE;
                break;
            }

            open_file_t* of_write = open_file_alloc(OPEN_FILE_PIPE, 0, O_WRONLY);
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

            me->fds[fd0] = of_read;
            me->fd_flags[fd0] = 0;
            me->fds[fd1] = of_write;
            me->fd_flags[fd1] = 0;

            user_fds[0] = fd0;
            user_fds[1] = fd1;
            regs->eax = 0;
            break;
        }

        case SYS_EXEC: {
            const char* name = (const char*) regs->ebx;
            const char* const* uargv = (const char* const*) regs->ecx;

            int err = validate_user_string(name);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* vn = 0;
            if (vfs_resolve_path(name, me->cwd, &vn) != 0 || !vn) {
                char bin_path[64];
                bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
                int l = 0;
                while (name[l] && l < 50) {
                    bin_path[5 + l] = name[l];
                    l++;
                }
                bin_path[5 + l] = '\0';
                if (vfs_resolve_path(bin_path, me->cwd, &vn) != 0 || !vn) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
            }

            /* Copy arguments from user space before tearing down address space */
            int argc = 0;
            char kargv_buf[16][64];
            const char* kargv[17];
            if (uargv) {
                int bad = 0;
                while (argc < 16) {
                    if (validate_user_buffer(&uargv[argc], sizeof(char*), 0) != 0) {
                        bad = 1; break;
                    }
                    const char* uarg = uargv[argc];
                    if (!uarg) break;
                    if (validate_user_string(uarg) != 0) {
                        bad = 1; break;
                    }
                    int j = 0;
                    while (uarg[j] && j < 63) {
                        kargv_buf[argc][j] = uarg[j];
                        j++;
                    }
                    kargv_buf[argc][j] = '\0';
                    kargv[argc] = kargv_buf[argc];
                    argc++;
                }
                if (bad) {
                    vnode_unref(vn);
                    regs->eax = (uint32_t) -EFAULT;
                    break;
                }
            }
            if (argc == 0) {
                int j = 0;
                while (name[j] && j < 63) {
                    kargv_buf[0][j] = name[j];
                    j++;
                }
                kargv_buf[0][j] = '\0';
                kargv[0] = kargv_buf[0];
                argc = 1;
            }
            kargv[argc] = 0;

            uint32_t alloc_size = ((vn->size + 511) / 512) * 512;
            uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
            if (!buf) { vnode_unref(vn); regs->eax = (uint32_t)-1; break; }

            int n = vn->ops->read(vn, 0, buf, vn->size);
            vnode_unref(vn);
            if (n <= 0) { kfree(buf); regs->eax = (uint32_t)-1; break; }

            address_space_t new_as = vmm_create_address_space();
            if (!new_as.directory) { kfree(buf); regs->eax = (uint32_t)-1; break; }

            uint32_t entry, stack_top;
            if (elf_load_into(buf, (uint32_t) n, &new_as, argc, kargv, &entry, &stack_top) != 0) {
                kfree(buf);
                vmm_destroy_address_space(&new_as);
                regs->eax = (uint32_t)-1;
                break;
            }
            kfree(buf);

            me = scheduler_current();

            /* Close file descriptors marked with FD_CLOEXEC */
            for (int fd = 0; fd < MAX_FDS; fd++) {
                if (me->fds[fd] && (me->fd_flags[fd] & FD_CLOEXEC)) {
                    open_file_t* of = me->fds[fd];
                    me->fds[fd] = 0;
                    me->fd_flags[fd] = 0;
                    vfs_close(of);
                }
            }

            vmm_destroy_address_space(&me->as);
            me->as = new_as;
            me->entry_point = entry;
            me->user_stack_top = stack_top;
            me->heap_end = HEAP_BASE;
            me->heap_mapped_up_to = HEAP_BASE;

            int i = 0; for (; name[i] && i < 31; i++) me->name[i] = name[i]; me->name[i] = '\0';

            vmm_switch(&me->as);
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

                for (int i = 0; i < MAX_PROCESSES; i++) {
                    process_t* child = process_table_entry(i);
                    if (!child || child->state == PROC_UNUSED) continue;
                    if (child->ppid != me->pid) continue;

                    if (pid == -1 || child->pid == pid) {
                        has_children = 1;
                        if (child->state == PROC_ZOMBIE) {
                            zombie = child;
                            break;
                        }
                    }
                }

                if (!has_children) {
                    regs->eax = (uint32_t) -ECHILD;
                    break;
                }

                if (zombie) {
                    int reaped_pid = zombie->pid;
                    int exit_code = zombie->exit_code;
                    if (user_status) {
                        *user_status = (exit_code & 0xff) << 8;
                    }
                    process_destroy(zombie);
                    regs->eax = (uint32_t) reaped_pid;
                    break;
                }

                if (options & WNOHANG) {
                    regs->eax = 0;
                    break;
                }

                scheduler_wait_for(pid);
            }
            break;
        }

        case SYS_SBRK: {
            int32_t increment = (int32_t) regs->ebx;
            process_t* me = scheduler_current();

            uint32_t old_break = me->heap_end;
            uint32_t new_break = old_break + increment;
            int failed = 0;

            if (increment > 0) {
                if (new_break > HEAP_MAX || new_break < old_break) {
                    failed = 1;
                } else {
                    uint32_t target = (new_break + 4095u) & ~4095u;
                    while (!failed && me->heap_mapped_up_to < target) {
                        uint32_t frame_phys = vmm_map_user_page(&me->as, me->heap_mapped_up_to);
                        if (!frame_phys) { failed = 1; break; }
                        uint8_t* frame = (uint8_t*) frame_phys;
                        for (int i = 0; i < 4096; i++) frame[i] = 0;
                        me->heap_mapped_up_to += 4096;
                    }
                }
            } else if (new_break < HEAP_BASE) {
                new_break = HEAP_BASE;
            }

            if (failed) { regs->eax = (uint32_t)-1; break; }

            me->heap_end = new_break;
            regs->eax = old_break;
            break;
        }

        case SYS_SYNC: {
            regs->eax = (uint32_t) bcache_sync();
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
            struct stat* st = (struct stat*) regs->ecx;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            err = validate_user_buffer(st, sizeof(struct stat), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* vn = 0;
            int ret = vfs_resolve_path(path, me->cwd, &vn);
            if (ret != 0 || !vn) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }

            ret = vfs_stat(vn, st);
            vnode_unref(vn);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_FSTAT: {
            int fd = (int) regs->ebx;
            struct stat* st = (struct stat*) regs->ecx;

            int err = validate_user_buffer(st, sizeof(struct stat), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            open_file_t* of = me->fds[fd];
            if (of->type == OPEN_FILE_CONSOLE) {
                st->st_dev = 0; st->st_ino = 0; st->st_mode = S_IFCHR | 0666;
                st->st_nlink = 1; st->st_size = 0; st->st_blksize = 512; st->st_blocks = 0;
                regs->eax = 0;
                break;
            }
            if (of->type == OPEN_FILE_VNODE && of->vnode) {
                regs->eax = (uint32_t) vfs_stat(of->vnode, st);
                break;
            }
            regs->eax = (uint32_t) -EBADF;
            break;
        }

        case SYS_GETDENTS: {
            int fd = (int) regs->ebx;
            struct dirent* dirp = (struct dirent*) regs->ecx;
            uint32_t count = regs->edx;

            if (count < sizeof(struct dirent)) {
                regs->eax = (uint32_t) -EINVAL;
                break;
            }

            int err = validate_user_buffer(dirp, sizeof(struct dirent), 1);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t) -EBADF;
                break;
            }

            int ret = vfs_readdir(me->fds[fd], dirp);
            if (ret == 1) {
                regs->eax = (uint32_t) sizeof(struct dirent);
            } else if (ret == 0) {
                regs->eax = 0; /* EOF */
            } else {
                regs->eax = (uint32_t) ret;
            }
            break;
        }

        case SYS_MKDIR: {
            const char* path = (const char*) regs->ebx;
            int mode = (int) regs->ecx;
            (void) mode;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* parent = 0;
            char child[64];
            int ret = vfs_resolve_parent(path, me->cwd, &parent, child, sizeof(child));
            if (ret != 0 || !parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
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

        case SYS_RMDIR: {
            const char* path = (const char*) regs->ebx;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* parent = 0;
            char child[64];
            int ret = vfs_resolve_parent(path, me->cwd, &parent, child, sizeof(child));
            if (ret != 0 || !parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (!parent->ops || !parent->ops->rmdir) {
                vnode_unref(parent);
                regs->eax = (uint32_t) -ENOSYS;
                break;
            }
            ret = parent->ops->rmdir(parent, child);
            vnode_unref(parent);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_UNLINK: {
            const char* path = (const char*) regs->ebx;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* parent = 0;
            char child[64];
            int ret = vfs_resolve_parent(path, me->cwd, &parent, child, sizeof(child));
            if (ret != 0 || !parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            if (!parent->ops || !parent->ops->unlink) {
                vnode_unref(parent);
                regs->eax = (uint32_t) -ENOSYS;
                break;
            }
            ret = parent->ops->unlink(parent, child);
            vnode_unref(parent);
            regs->eax = (uint32_t) ret;
            break;
        }

        case SYS_RENAME: {
            const char* oldpath = (const char*) regs->ebx;
            const char* newpath = (const char*) regs->ecx;

            int err = validate_user_string(oldpath);
            if (err != 0) { regs->eax = (uint32_t) err; break; }
            err = validate_user_string(newpath);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t *old_parent = 0, *new_parent = 0;
            char old_child[64], new_child[64];
            int ret = vfs_resolve_parent(oldpath, me->cwd, &old_parent, old_child, sizeof(old_child));
            if (ret != 0 || !old_parent) {
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
            }
            ret = vfs_resolve_parent(newpath, me->cwd, &new_parent, new_child, sizeof(new_child));
            if (ret != 0 || !new_parent) {
                vnode_unref(old_parent);
                regs->eax = (uint32_t) (ret ? ret : -ENOENT);
                break;
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

        case SYS_CHDIR: {
            const char* path = (const char*) regs->ebx;

            int err = validate_user_string(path);
            if (err != 0) { regs->eax = (uint32_t) err; break; }

            process_t* me = scheduler_current();
            vnode_t* vn = 0;
            int ret = vfs_resolve_path(path, me->cwd, &vn);
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

            vfs_path_canonical(path, me->cwd, me->cwd, sizeof(me->cwd));
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
            regs->eax = (uint32_t) buf;
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

        case SYS_DUP2: {
            int oldfd = (int) regs->ebx;
            int newfd = (int) regs->ecx;
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
                regs->eax = (uint32_t) newfd;
                break;
            }
            if (me->fds[newfd]) {
                open_file_t* old_of = me->fds[newfd];
                me->fds[newfd] = 0;
                me->fd_flags[newfd] = 0;
                vfs_close(old_of);
            }
            me->fds[newfd] = me->fds[oldfd];
            me->fd_flags[newfd] = 0;
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
            } else if (cmd == F_DUPFD) {
                int min_fd = arg >= 0 ? arg : 0;
                int new_fd = -1;
                for (int i = min_fd; i < MAX_FDS; i++) {
                    if (!me->fds[i]) {
                        new_fd = i;
                        break;
                    }
                }
                if (new_fd < 0) {
                    regs->eax = (uint32_t) -EMFILE;
                } else {
                    me->fds[new_fd] = me->fds[fd];
                    me->fd_flags[new_fd] = 0;
                    open_file_ref(me->fds[new_fd]);
                    regs->eax = (uint32_t) new_fd;
                }
            } else {
                regs->eax = (uint32_t) -EINVAL;
            }
            break;
        }

        case SYS_SLEEP: {
            uint32_t sec = regs->ebx;
            uint32_t start = timer_get_ticks();
            uint32_t target = start + sec * 100;
            while (timer_get_ticks() < target) {
                scheduler_yield();
            }
            regs->eax = 0;
            break;
        }

        case SYS_TICKS: {
            regs->eax = timer_get_ticks();
            break;
        }

        default:
            terminal_writestring("[warn] unknown syscall\n");
            regs->eax = (uint32_t) -ENOSYS;
            break;
    }
}

void syscall_install(void) {
    /* 0xEE = present, DPL=3 (so ring-3 code is allowed to `int 0x80`
       without a general protection fault), 32-bit interrupt gate */
    idt_set_gate(128, (uint32_t) isr128, 0x08, 0xEE);
    register_interrupt_handler(128, &syscall_handler);
}

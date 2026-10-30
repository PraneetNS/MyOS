#include "procfs.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "pmm.h"
#include "rtc.h"
#include "vma.h"
#include "errno.h"

#include "socket.h"
#include "agent.h"

static vnode_t proc_root_vnode;
static vnode_t proc_meminfo_vnode;
static vnode_t proc_uptime_vnode;
static vnode_t proc_version_vnode;
static vnode_t proc_cpuinfo_vnode;
static vnode_t proc_mounts_vnode;
static vnode_t proc_global_cmdline_vnode;
static vnode_t proc_net_dir_vnode;
static vnode_t proc_net_unix_vnode;
static vnode_t proc_self_dir_vnode;
static vnode_t proc_self_status_vnode;
static vnode_t proc_self_cmdline_vnode;
static vnode_t proc_self_fd_dir_vnode;

static vnode_t proc_dir_vnodes[MAX_PROCESSES];
static vnode_t proc_status_vnodes[MAX_PROCESSES];
static vnode_t proc_cmdline_vnodes[MAX_PROCESSES];
static vnode_t proc_fd_dir_vnodes[MAX_PROCESSES];

static vnode_t proc_agents_dir_vnode;
static vnode_t proc_agents_log_vnode;
static vnode_t proc_agent_dir_vnodes[MAX_AGENTS];
static vnode_t proc_agent_status_vnodes[MAX_AGENTS];
static vnode_t proc_agent_caps_vnodes[MAX_AGENTS];
static vnode_t proc_agent_budget_vnodes[MAX_AGENTS];

/* String formatting utilities */
static void append_char(char* buf, int* pos, int max, char c) {
    if (*pos < max - 1) {
        buf[(*pos)++] = c;
        buf[*pos] = '\0';
    }
}

static void append_str(char* buf, int* pos, int max, const char* s) {
    if (!s) return;
    while (*s && *pos < max - 1) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

static void append_uint(char* buf, int* pos, int max, uint32_t val) {
    char tmp[16];
    int i = 0;
    if (val == 0) {
        append_char(buf, pos, max, '0');
        return;
    }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        append_char(buf, pos, max, tmp[--i]);
    }
}

static void append_int(char* buf, int* pos, int max, int val) {
    if (val < 0) {
        append_char(buf, pos, max, '-');
        val = -val;
    }
    append_uint(buf, pos, max, (uint32_t)val);
}

static void append_uint_pad2(char* buf, int* pos, int max, uint32_t val) {
    if (val < 10) {
        append_char(buf, pos, max, '0');
    }
    append_uint(buf, pos, max, val);
}

static void append_hex(char* buf, int* pos, int max, uint32_t val, int width) {
    char tmp[16];
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = width - 1; i >= 0; i--) {
        tmp[i] = hex_chars[val & 0xF];
        val >>= 4;
    }
    for (int i = 0; i < width; i++) {
        append_char(buf, pos, max, tmp[i]);
    }
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int is_all_digits(const char* s) {
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static int parse_int(const char* s) {
    int res = 0;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res;
}

static int procfs_read_helper(const char* text, int len, uint32_t offset, uint8_t* buf, uint32_t count) {
    if (offset >= (uint32_t)len) return 0;
    uint32_t avail = (uint32_t)len - offset;
    if (count > avail) count = avail;
    for (uint32_t i = 0; i < count; i++) {
        buf[i] = (uint8_t)text[offset + i];
    }
    return (int)count;
}

/* /proc/meminfo */
static int proc_meminfo_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    char text[256];
    int pos = 0;
    uint32_t total_frames = pmm_total_count();
    uint32_t free_frames  = pmm_free_frame_count();
    uint32_t total_kb     = total_frames * 4;
    uint32_t free_kb      = free_frames * 4;

    append_str(text, &pos, sizeof(text), "MemTotal:       ");
    append_uint(text, &pos, sizeof(text), total_kb);
    append_str(text, &pos, sizeof(text), " kB\nMemFree:        ");
    append_uint(text, &pos, sizeof(text), free_kb);
    append_str(text, &pos, sizeof(text), " kB\nFramesTotal:    ");
    append_uint(text, &pos, sizeof(text), total_frames);
    append_str(text, &pos, sizeof(text), "\nFramesFree:     ");
    append_uint(text, &pos, sizeof(text), free_frames);
    append_str(text, &pos, sizeof(text), "\n");

    return procfs_read_helper(text, pos, offset, buf, count);
}

/* /proc/uptime */
static int proc_uptime_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    char text[64];
    int pos = 0;
    uint32_t ticks = timer_get_ticks();
    uint32_t idle_ticks = timer_get_idle_ticks();

    append_uint(text, &pos, sizeof(text), ticks / 100);
    append_char(text, &pos, sizeof(text), '.');
    append_uint_pad2(text, &pos, sizeof(text), ticks % 100);
    append_char(text, &pos, sizeof(text), ' ');
    append_uint(text, &pos, sizeof(text), idle_ticks / 100);
    append_char(text, &pos, sizeof(text), '.');
    append_uint_pad2(text, &pos, sizeof(text), idle_ticks % 100);
    append_char(text, &pos, sizeof(text), '\n');

    return procfs_read_helper(text, pos, offset, buf, count);
}

/* /proc/version */
static int proc_version_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    const char* text = "MyOS version 1.0.0-i386 (gcc 11.4.0) #1 SMP PREEMPT Thu Oct 15 2026 i686 GNU/Linux\n";
    int len = 0;
    while (text[len]) len++;
    return procfs_read_helper(text, len, offset, buf, count);
}

static inline void proc_cpuid(uint32_t code, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(code)
    );
}

/* /proc/cpuinfo */
static int proc_cpuinfo_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    char text[512];
    int pos = 0;

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    char vendor[13] = {0};
    proc_cpuid(0, &eax, &ebx, &ecx, &edx);
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;

    proc_cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t stepping = eax & 0x0F;
    uint32_t model = (eax >> 4) & 0x0F;
    uint32_t family = (eax >> 8) & 0x0F;

    append_str(text, &pos, sizeof(text), "processor\t: 0\n");
    append_str(text, &pos, sizeof(text), "vendor_id\t: ");
    append_str(text, &pos, sizeof(text), vendor);
    append_str(text, &pos, sizeof(text), "\ncpu family\t: ");
    append_uint(text, &pos, sizeof(text), family);
    append_str(text, &pos, sizeof(text), "\nmodel\t\t: ");
    append_uint(text, &pos, sizeof(text), model);
    append_str(text, &pos, sizeof(text), "\nstepping\t: ");
    append_uint(text, &pos, sizeof(text), stepping);
    append_str(text, &pos, sizeof(text), "\nflags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 mmx fxsr sse sse2\n");

    return procfs_read_helper(text, pos, offset, buf, count);
}

/* /proc/<pid>/status */
static int proc_status_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    int slot = (int)(uintptr_t)vn->fs_data;
    process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
    if (!p || p->state == PROC_UNUSED) return -ENOENT;

    char text[512];
    int pos = 0;

    const char* state_str = "R (running)";
    if (p->state == PROC_READY) {
        state_str = "R (ready)";
    } else if (p->state == PROC_WAITING) {
        if (p->is_stopped) state_str = "T (stopped)";
        else state_str = "S (sleeping)";
    } else if (p->state == PROC_ZOMBIE) {
        state_str = "Z (zombie)";
    }

    uint32_t vm_pages = 0;
    vma_t* v = p->vma_list;
    while (v) {
        vm_pages += (v->end - v->start) / 4096;
        v = v->next;
    }

    int fds_count = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) fds_count++;
    }

    append_str(text, &pos, sizeof(text), "Name:\t");
    append_str(text, &pos, sizeof(text), p->name);
    append_str(text, &pos, sizeof(text), "\nState:\t");
    append_str(text, &pos, sizeof(text), state_str);
    append_str(text, &pos, sizeof(text), "\nPid:\t");
    append_int(text, &pos, sizeof(text), p->pid);
    append_str(text, &pos, sizeof(text), "\nPPid:\t");
    append_int(text, &pos, sizeof(text), p->ppid);
    append_str(text, &pos, sizeof(text), "\nPgid:\t");
    append_int(text, &pos, sizeof(text), p->pgid);
    append_str(text, &pos, sizeof(text), "\nSid:\t");
    append_int(text, &pos, sizeof(text), p->sid);
    append_str(text, &pos, sizeof(text), "\nTicks:\t");
    append_uint(text, &pos, sizeof(text), p->cpu_ticks);
    append_str(text, &pos, sizeof(text), "\nVmPages:\t");
    append_uint(text, &pos, sizeof(text), vm_pages);
    append_str(text, &pos, sizeof(text), "\nFDSize:\t");
    append_int(text, &pos, sizeof(text), fds_count);
    append_str(text, &pos, sizeof(text), "\n");

    return procfs_read_helper(text, pos, offset, buf, count);
}

/* /proc/<pid>/cmdline */
static int proc_cmdline_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    int slot = (int)(uintptr_t)vn->fs_data;
    process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
    if (!p || p->state == PROC_UNUSED) return -ENOENT;

    char text[64];
    int pos = 0;
    append_str(text, &pos, sizeof(text), p->name);
    append_char(text, &pos, sizeof(text), '\n');

    return procfs_read_helper(text, pos, offset, buf, count);
}

/* /proc/cmdline */
static int proc_global_cmdline_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    extern const char* kernel_get_cmdline(void);
    const char* s = kernel_get_cmdline();
    if (!s) s = "mode=text\n";
    int len = 0;
    while (s[len]) len++;
    return procfs_read_helper(s, len, offset, buf, count);
}

/* Common stat for regular files in procfs */
static int procfs_file_stat(vnode_t* vn, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    st->st_mtime = rtc_get_epoch();
    st->st_ctime = st->st_mtime;
    st->st_atime = st->st_mtime;
    return 0;
}

static const struct fs_ops global_cmdline_ops = {
    .read = proc_global_cmdline_read,
    .stat = procfs_file_stat
};

/* Common stat for directories in procfs */
static int procfs_dir_stat(vnode_t* vn, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    st->st_mtime = rtc_get_epoch();
    st->st_ctime = st->st_mtime;
    st->st_atime = st->st_mtime;
    return 0;
}

static const struct fs_ops meminfo_ops = {
    .read = proc_meminfo_read,
    .stat = procfs_file_stat
};

static const struct fs_ops uptime_ops = {
    .read = proc_uptime_read,
    .stat = procfs_file_stat
};

static const struct fs_ops version_ops = {
    .read = proc_version_read,
    .stat = procfs_file_stat
};

static int proc_mounts_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    char text[1024];
    int pos = 0;

    for (int i = 0; i < 16; i++) {
        char dev[32];
        char path[64];
        char fstype[16];
        if (vfs_get_mount_info(i, dev, path, fstype)) {
            append_str(text, &pos, sizeof(text), dev);
            append_char(text, &pos, sizeof(text), ' ');
            append_str(text, &pos, sizeof(text), path);
            append_char(text, &pos, sizeof(text), ' ');
            append_str(text, &pos, sizeof(text), fstype);
            append_str(text, &pos, sizeof(text), " rw,relatime 0 0\n");
        }
    }

    return procfs_read_helper(text, pos, offset, buf, count);
}

static const struct fs_ops cpuinfo_ops = {
    .read = proc_cpuinfo_read,
    .stat = procfs_file_stat
};

static const struct fs_ops mounts_ops = {
    .read = proc_mounts_read,
    .stat = procfs_file_stat
};

static const struct fs_ops status_ops = {
    .read = proc_status_read,
    .stat = procfs_file_stat
};

static const struct fs_ops cmdline_ops = {
    .read = proc_cmdline_read,
    .stat = procfs_file_stat
};

/* /proc/net and /proc/net/unix operations */
static int net_unix_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    char text[2048];
    int pos = 0;

    append_str(text, &pos, sizeof(text), "Num       RefCount Protocol Flags    Type St Inode Path\n");
    socket_t* s = socket_get_list_head();
    while (s && pos < (int)sizeof(text) - 128) {
        append_hex(text, &pos, sizeof(text), (uint32_t)(uintptr_t)s, 8);
        append_str(text, &pos, sizeof(text), ": ");
        append_hex(text, &pos, sizeof(text), (uint32_t)s->refcount, 8);
        append_char(text, &pos, sizeof(text), ' ');
        append_hex(text, &pos, sizeof(text), (uint32_t)s->protocol, 8);
        append_char(text, &pos, sizeof(text), ' ');
        append_hex(text, &pos, sizeof(text), (uint32_t)s->flags, 8);
        append_char(text, &pos, sizeof(text), ' ');
        append_hex(text, &pos, sizeof(text), (uint32_t)s->type, 4);
        append_char(text, &pos, sizeof(text), ' ');
        append_hex(text, &pos, sizeof(text), (uint32_t)s->state, 2);
        append_char(text, &pos, sizeof(text), ' ');
        append_uint(text, &pos, sizeof(text), s->inode);
        append_char(text, &pos, sizeof(text), ' ');
        if (s->sun_path[0]) {
            append_str(text, &pos, sizeof(text), s->sun_path);
        } else if (s->peer_path[0]) {
            append_str(text, &pos, sizeof(text), s->peer_path);
        }
        append_char(text, &pos, sizeof(text), '\n');
        s = s->next_all;
    }

    return procfs_read_helper(text, pos, offset, buf, count);
}

static const struct fs_ops net_unix_ops = {
    .read = net_unix_read,
    .stat = procfs_file_stat
};

static int proc_net_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    (void) dir;
    if (streq(name, ".")) {
        *out = &proc_net_dir_vnode;
        vnode_ref(&proc_net_dir_vnode);
        return 0;
    }
    if (streq(name, "..")) {
        *out = &proc_root_vnode;
        vnode_ref(&proc_root_vnode);
        return 0;
    }
    if (streq(name, "unix")) {
        *out = &proc_net_unix_vnode;
        vnode_ref(&proc_net_unix_vnode);
        return 0;
    }
    return -ENOENT;
}

static int proc_net_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino = 300; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "unix");
        return 1;
    }
    return 0;
}

static const struct fs_ops proc_net_dir_ops = {
    .lookup = proc_net_dir_lookup,
    .readdir = proc_net_dir_readdir,
    .stat = procfs_dir_stat
};

/* /proc/<pid>/fd operations */
static int proc_fd_entry_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    uint32_t val = (uint32_t)(uintptr_t) vn->fs_data;
    int slot = (int)(int16_t)(val >> 16);
    int fd = (int)(int16_t)(val & 0xFFFF);

    process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
    if (!p || fd < 0 || fd >= MAX_FDS || !p->fds[fd]) return -EBADF;

    open_file_t* of = p->fds[fd];
    char text[128];
    int pos = 0;

    if (of->type == OPEN_FILE_SOCKET && of->sock) {
        socket_t* s = (socket_t*) of->sock;
        append_str(text, &pos, sizeof(text), "socket:[");
        append_uint(text, &pos, sizeof(text), s->inode);
        append_str(text, &pos, sizeof(text), "]\n");
    } else if (of->type == OPEN_FILE_PIPE && of->pipe) {
        append_str(text, &pos, sizeof(text), "pipe:[");
        append_uint(text, &pos, sizeof(text), (uint32_t)(uintptr_t)of->pipe);
        append_str(text, &pos, sizeof(text), "]\n");
    } else if (of->type == OPEN_FILE_CONSOLE) {
        append_str(text, &pos, sizeof(text), "/dev/console\n");
    } else if (of->type == OPEN_FILE_VNODE && of->vnode) {
        append_str(text, &pos, sizeof(text), "vnode\n");
    } else {
        append_str(text, &pos, sizeof(text), "unknown\n");
    }

    return procfs_read_helper(text, pos, offset, buf, count);
}

static int proc_fd_entry_stat(vnode_t* vn, struct stat* st) {
    uint32_t val = (uint32_t)(uintptr_t) vn->fs_data;
    int slot = (int)(int16_t)(val >> 16);
    int fd = (int)(int16_t)(val & 0xFFFF);

    process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
    if (!p || fd < 0 || fd >= MAX_FDS || !p->fds[fd]) return -EBADF;

    st->st_dev = 0;
    st->st_ino = 3000 + fd;
    st->st_mode = S_IFLNK | 0777;
    st->st_nlink = 1;
    st->st_size = 64;
    st->st_blksize = 512;
    st->st_blocks = 1;
    st->st_atime = 0;
    st->st_mtime = 0;
    st->st_ctime = 0;
    return 0;
}

static const struct fs_ops proc_fd_entry_ops = {
    .read = proc_fd_entry_read,
    .stat = proc_fd_entry_stat
};

static int proc_fd_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    int slot = (int)(uintptr_t)dir->fs_data;
    if (streq(name, ".")) {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (streq(name, "..")) {
        vnode_t* parent = (slot < 0) ? &proc_self_dir_vnode : &proc_dir_vnodes[slot];
        *out = parent;
        vnode_ref(parent);
        return 0;
    }
    if (is_all_digits(name)) {
        int fd = parse_int(name);
        process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
        if (p && fd >= 0 && fd < MAX_FDS && p->fds[fd]) {
            uint32_t data = ((uint32_t)(int16_t)slot << 16) | (uint32_t)(uint16_t)fd;
            vnode_t* vn = vnode_alloc(VNODE_FILE, &proc_fd_entry_ops, (void*)(uintptr_t)data);
            if (!vn) return -ENOMEM;
            vn->parent = dir;
            vnode_ref(dir);
            *out = vn;
            return 0;
        }
    }
    return -ENOENT;
}

static int proc_fd_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    int slot = (int)(uintptr_t)dir->fs_data;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }

    process_t* p = (slot < 0) ? scheduler_current() : process_table_entry(slot);
    if (!p) return 0;

    uint32_t target_idx = index - 2;
    uint32_t active_count = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            if (active_count == target_idx) {
                entry->d_ino = 2000 + i;
                entry->d_type = 1;
                entry->d_size = 0;
                int pos = 0;
                append_uint(entry->d_name, &pos, sizeof(entry->d_name), (uint32_t)i);
                return 1;
            }
            active_count++;
        }
    }
    return 0;
}

static const struct fs_ops proc_fd_dir_ops = {
    .lookup = proc_fd_dir_lookup,
    .readdir = proc_fd_dir_readdir,
    .stat = procfs_dir_stat
};

/* /proc/<pid> operations */
static int proc_pid_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    int slot = (int)(uintptr_t)dir->fs_data;
    if (streq(name, ".")) {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (streq(name, "..")) {
        *out = &proc_root_vnode;
        vnode_ref(&proc_root_vnode);
        return 0;
    }
    if (streq(name, "status")) {
        vnode_t* target = (slot < 0) ? &proc_self_status_vnode : &proc_status_vnodes[slot];
        *out = target;
        vnode_ref(target);
        return 0;
    }
    if (streq(name, "cmdline")) {
        vnode_t* target = (slot < 0) ? &proc_self_cmdline_vnode : &proc_cmdline_vnodes[slot];
        *out = target;
        vnode_ref(target);
        return 0;
    }
    if (streq(name, "fd")) {
        vnode_t* target = (slot < 0) ? &proc_self_fd_dir_vnode : &proc_fd_dir_vnodes[slot];
        *out = target;
        vnode_ref(target);
        return 0;
    }
    return -ENOENT;
}

static int proc_pid_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino = 3; entry->d_type = 1; entry->d_size = 0;
        append_str(entry->d_name, &(int){0}, sizeof(entry->d_name), "status");
        return 1;
    }
    if (index == 3) {
        entry->d_ino = 4; entry->d_type = 1; entry->d_size = 0;
        append_str(entry->d_name, &(int){0}, sizeof(entry->d_name), "cmdline");
        return 1;
    }
    if (index == 4) {
        entry->d_ino = 5; entry->d_type = 2; entry->d_size = 0;
        append_str(entry->d_name, &(int){0}, sizeof(entry->d_name), "fd");
        return 1;
    }
    return 0;
}

static const struct fs_ops proc_pid_dir_ops = {
    .lookup = proc_pid_dir_lookup,
    .readdir = proc_pid_dir_readdir,
    .stat = procfs_dir_stat
};

/* /proc/agents/log read */
static int proc_agents_log_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    return agent_audit_read(offset, buf, count);
}

static const struct fs_ops agents_log_ops = {
    .read = proc_agents_log_read,
    .stat = procfs_file_stat
};

static agent_info_t s_proc_agent_info;
static char s_proc_agent_text[2048];

/* /proc/agents/<id>/status read */
static int proc_agent_status_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    int slot = (int)(uintptr_t)vn->fs_data;
    if (agent_get_info(slot + 1, &s_proc_agent_info) != 0) return -ENOENT;

    int pos = 0;
    const char* st_str = "UNUSED";
    if (s_proc_agent_info.state == AGENT_STATE_RUNNING) st_str = "RUNNING";
    else if (s_proc_agent_info.state == AGENT_STATE_SUSPENDED) st_str = "SUSPENDED";
    else if (s_proc_agent_info.state == AGENT_STATE_KILLED) st_str = "KILLED";

    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "Id:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), (uint32_t)s_proc_agent_info.id);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nName:\t");
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.name);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nPrincipal:\t");
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.principal);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nState:\t");
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), st_str);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nOwnerPid:\t");
    append_int(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.owner_pid);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nParentId:\t");
    append_int(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.parent_agent_id);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nProcs:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.usage.procs_count);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nCpuTicks:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.usage.cpu_ticks);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMemPages:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.usage.mem_pages);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nTokens:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.usage.tokens_used);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\n");

    return procfs_read_helper(s_proc_agent_text, pos, offset, buf, count);
}

static const struct fs_ops agent_status_ops = {
    .read = proc_agent_status_read,
    .stat = procfs_file_stat
};

/* /proc/agents/<id>/caps read */
static int proc_agent_caps_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    int slot = (int)(uintptr_t)vn->fs_data;
    if (agent_get_info(slot + 1, &s_proc_agent_info) != 0) return -ENOENT;

    int pos = 0;
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "Mask:\t0x");
    append_hex(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.mask, 8);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nFsRead:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_fs_read && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.fs_read[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nFsWrite:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_fs_write && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.fs_write[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nExec:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_exec && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.exec[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nConnect:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_connect && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.connect[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nBusCall:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_bus_call && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.bus_call[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nBusProvide:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_bus_provide && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.bus_provide[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nWindowObserve:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_window_observe && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.window_observe[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nWindowAct:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_window_act && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.window_act[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMemRead:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_mem_read && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.mem_read[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMemWrite:\t");
    for (uint32_t i = 0; i < s_proc_agent_info.caps.num_mem_write && i < MAX_PATH_RULES; i++) {
        if (i > 0) append_char(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), ',');
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.caps.mem_write[i].prefix);
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nWindowSecret:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), (s_proc_agent_info.caps.mask & CAP_WINDOW_SECRET) ? 1 : 0);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nWindowRaw:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), (s_proc_agent_info.caps.mask & CAP_WINDOW_RAW) ? 1 : 0);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nModelBackends:\t");
    uint32_t mb = s_proc_agent_info.caps.model_backends;
    if (mb == 0 || mb == MODEL_BACKEND_ALL) {
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "local,host");
    } else if (mb == MODEL_BACKEND_LOCAL) {
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "local");
    } else if (mb == MODEL_BACKEND_HOST) {
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "host");
    } else {
        append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "none");
    }
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\n");

    return procfs_read_helper(s_proc_agent_text, pos, offset, buf, count);
}

static const struct fs_ops agent_caps_ops = {
    .read = proc_agent_caps_read,
    .stat = procfs_file_stat
};

/* /proc/agents/<id>/budget read */
static int proc_agent_budget_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    int slot = (int)(uintptr_t)vn->fs_data;
    if (agent_get_info(slot + 1, &s_proc_agent_info) != 0) return -ENOENT;

    int pos = 0;
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "MaxCpuTicks:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.budget.max_cpu_ticks);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMaxMemPages:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.budget.max_mem_pages);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMaxProcs:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.budget.max_procs);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nMaxTokens:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.budget.max_tokens);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\nTokensUsed:\t");
    append_uint(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), s_proc_agent_info.usage.tokens_used);
    append_str(s_proc_agent_text, &pos, sizeof(s_proc_agent_text), "\n");

    return procfs_read_helper(s_proc_agent_text, pos, offset, buf, count);
}

static const struct fs_ops agent_budget_ops = {
    .read = proc_agent_budget_read,
    .stat = procfs_file_stat
};

/* /proc/agents/<id> directory ops */
static int proc_agent_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    int slot = (int)(uintptr_t)dir->fs_data;
    if (streq(name, ".")) {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (streq(name, "..")) {
        *out = &proc_agents_dir_vnode;
        vnode_ref(&proc_agents_dir_vnode);
        return 0;
    }
    if (streq(name, "status")) {
        *out = &proc_agent_status_vnodes[slot];
        vnode_ref(&proc_agent_status_vnodes[slot]);
        return 0;
    }
    if (streq(name, "caps")) {
        *out = &proc_agent_caps_vnodes[slot];
        vnode_ref(&proc_agent_caps_vnodes[slot]);
        return 0;
    }
    if (streq(name, "budget")) {
        *out = &proc_agent_budget_vnodes[slot];
        vnode_ref(&proc_agent_budget_vnodes[slot]);
        return 0;
    }
    return -ENOENT;
}

static int proc_agent_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino = 3; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "status");
        return 1;
    }
    if (index == 3) {
        entry->d_ino = 4; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "caps");
        return 1;
    }
    if (index == 4) {
        entry->d_ino = 5; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "budget");
        return 1;
    }
    return 0;
}

static const struct fs_ops proc_agent_dir_ops = {
    .lookup = proc_agent_dir_lookup,
    .readdir = proc_agent_dir_readdir,
    .stat = procfs_dir_stat
};

/* /proc/agents directory ops */
static int proc_agents_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    if (streq(name, ".")) {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (streq(name, "..")) {
        *out = &proc_root_vnode;
        vnode_ref(&proc_root_vnode);
        return 0;
    }
    if (streq(name, "log")) {
        *out = &proc_agents_log_vnode;
        vnode_ref(&proc_agents_log_vnode);
        return 0;
    }
    if (is_all_digits(name)) {
        int id = parse_int(name);
        agent_t* ag = agent_get_by_id(id);
        if (ag) {
            *out = &proc_agent_dir_vnodes[id - 1];
            vnode_ref(&proc_agent_dir_vnodes[id - 1]);
            return 0;
        }
    }
    return -ENOENT;
}

static int proc_agents_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino = 3; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "log");
        return 1;
    }

    uint32_t target_idx = index - 3;
    uint32_t active_count = 0;
    for (int i = 1; i <= MAX_AGENTS; i++) {
        agent_t* ag = agent_get_by_id(i);
        if (ag) {
            if (active_count == target_idx) {
                entry->d_ino = 500 + i;
                entry->d_type = 2; /* directory */
                entry->d_size = 0;
                int pos = 0;
                append_uint(entry->d_name, &pos, sizeof(entry->d_name), (uint32_t)i);
                return 1;
            }
            active_count++;
        }
    }
    return 0;
}

static const struct fs_ops proc_agents_dir_ops = {
    .lookup = proc_agents_dir_lookup,
    .readdir = proc_agents_dir_readdir,
    .stat = procfs_dir_stat
};

/* /proc root directory operations */
static int proc_root_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    if (streq(name, ".") || streq(name, "..")) {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (streq(name, "meminfo")) {
        *out = &proc_meminfo_vnode;
        vnode_ref(&proc_meminfo_vnode);
        return 0;
    }
    if (streq(name, "uptime")) {
        *out = &proc_uptime_vnode;
        vnode_ref(&proc_uptime_vnode);
        return 0;
    }
    if (streq(name, "version")) {
        *out = &proc_version_vnode;
        vnode_ref(&proc_version_vnode);
        return 0;
    }
    if (streq(name, "cpuinfo")) {
        *out = &proc_cpuinfo_vnode;
        vnode_ref(&proc_cpuinfo_vnode);
        return 0;
    }
    if (streq(name, "mounts")) {
        *out = &proc_mounts_vnode;
        vnode_ref(&proc_mounts_vnode);
        return 0;
    }
    if (streq(name, "net")) {
        *out = &proc_net_dir_vnode;
        vnode_ref(&proc_net_dir_vnode);
        return 0;
    }
    if (streq(name, "self")) {
        *out = &proc_self_dir_vnode;
        vnode_ref(&proc_self_dir_vnode);
        return 0;
    }
    if (streq(name, "cmdline")) {
        *out = &proc_global_cmdline_vnode;
        vnode_ref(&proc_global_cmdline_vnode);
        return 0;
    }
    if (streq(name, "agents")) {
        *out = &proc_agents_dir_vnode;
        vnode_ref(&proc_agents_dir_vnode);
        return 0;
    }
    if (is_all_digits(name)) {
        int pid = parse_int(name);
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t* p = process_table_entry(i);
            if (p && p->state != PROC_UNUSED && p->state != PROC_EXITED && p->pid == pid) {
                *out = &proc_dir_vnodes[i];
                vnode_ref(&proc_dir_vnodes[i]);
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int proc_root_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (index == 0) {
        entry->d_ino = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino = 10; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "meminfo");
        return 1;
    }
    if (index == 3) {
        entry->d_ino = 11; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "uptime");
        return 1;
    }
    if (index == 4) {
        entry->d_ino = 13; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "version");
        return 1;
    }
    if (index == 5) {
        entry->d_ino = 14; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "cpuinfo");
        return 1;
    }
    if (index == 6) {
        entry->d_ino = 15; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "mounts");
        return 1;
    }
    if (index == 7) {
        entry->d_ino = 16; entry->d_type = 2; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "net");
        return 1;
    }
    if (index == 8) {
        entry->d_ino = 12; entry->d_type = 2; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "self");
        return 1;
    }
    if (index == 9) {
        entry->d_ino = 17; entry->d_type = 1; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "cmdline");
        return 1;
    }
    if (index == 10) {
        entry->d_ino = 18; entry->d_type = 2; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "agents");
        return 1;
    }

    /* index >= 11 corresponds to active processes */
    uint32_t target_idx = index - 11;
    uint32_t active_count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_table_entry(i);
        if (p && p->pid > 0 && p->state != PROC_UNUSED && p->state != PROC_EXITED) {
            if (active_count == target_idx) {
                entry->d_ino = 100 + p->pid;
                entry->d_type = 2; /* directory */
                entry->d_size = 0;
                int pos = 0;
                append_uint(entry->d_name, &pos, sizeof(entry->d_name), (uint32_t)p->pid);
                return 1;
            }
            active_count++;
        }
    }

    return 0;
}

static const struct fs_ops proc_root_ops = {
    .lookup = proc_root_lookup,
    .readdir = proc_root_readdir,
    .stat = procfs_dir_stat
};

void procfs_init(void) {
    proc_root_vnode.type = VNODE_DIR;
    proc_root_vnode.size = 0;
    proc_root_vnode.fs_data = 0;
    proc_root_vnode.ops = &proc_root_ops;
    proc_root_vnode.refcount = 1000;
    proc_root_vnode.parent = 0;

    proc_meminfo_vnode.type = VNODE_FILE;
    proc_meminfo_vnode.size = 0;
    proc_meminfo_vnode.fs_data = 0;
    proc_meminfo_vnode.ops = &meminfo_ops;
    proc_meminfo_vnode.refcount = 1000;
    proc_meminfo_vnode.parent = &proc_root_vnode;

    proc_uptime_vnode.type = VNODE_FILE;
    proc_uptime_vnode.size = 0;
    proc_uptime_vnode.fs_data = 0;
    proc_uptime_vnode.ops = &uptime_ops;
    proc_uptime_vnode.refcount = 1000;
    proc_uptime_vnode.parent = &proc_root_vnode;

    proc_version_vnode.type = VNODE_FILE;
    proc_version_vnode.size = 0;
    proc_version_vnode.fs_data = 0;
    proc_version_vnode.ops = &version_ops;
    proc_version_vnode.refcount = 1000;
    proc_version_vnode.parent = &proc_root_vnode;

    proc_cpuinfo_vnode.type = VNODE_FILE;
    proc_cpuinfo_vnode.size = 0;
    proc_cpuinfo_vnode.fs_data = 0;
    proc_cpuinfo_vnode.ops = &cpuinfo_ops;
    proc_cpuinfo_vnode.refcount = 1000;
    proc_cpuinfo_vnode.parent = &proc_root_vnode;

    proc_mounts_vnode.type = VNODE_FILE;
    proc_mounts_vnode.size = 0;
    proc_mounts_vnode.fs_data = 0;
    proc_mounts_vnode.ops = &mounts_ops;
    proc_mounts_vnode.refcount = 1000;
    proc_mounts_vnode.parent = &proc_root_vnode;

    proc_global_cmdline_vnode.type = VNODE_FILE;
    proc_global_cmdline_vnode.size = 0;
    proc_global_cmdline_vnode.fs_data = 0;
    proc_global_cmdline_vnode.ops = &global_cmdline_ops;
    proc_global_cmdline_vnode.refcount = 1000;
    proc_global_cmdline_vnode.parent = &proc_root_vnode;

    proc_net_dir_vnode.type = VNODE_DIR;
    proc_net_dir_vnode.size = 0;
    proc_net_dir_vnode.fs_data = 0;
    proc_net_dir_vnode.ops = &proc_net_dir_ops;
    proc_net_dir_vnode.refcount = 1000;
    proc_net_dir_vnode.parent = &proc_root_vnode;

    proc_net_unix_vnode.type = VNODE_FILE;
    proc_net_unix_vnode.size = 0;
    proc_net_unix_vnode.fs_data = 0;
    proc_net_unix_vnode.ops = &net_unix_ops;
    proc_net_unix_vnode.refcount = 1000;
    proc_net_unix_vnode.parent = &proc_net_dir_vnode;

    proc_self_dir_vnode.type = VNODE_DIR;
    proc_self_dir_vnode.size = 0;
    proc_self_dir_vnode.fs_data = (void*)(uintptr_t)-1;
    proc_self_dir_vnode.ops = &proc_pid_dir_ops;
    proc_self_dir_vnode.refcount = 1000;
    proc_self_dir_vnode.parent = &proc_root_vnode;

    proc_self_status_vnode.type = VNODE_FILE;
    proc_self_status_vnode.size = 0;
    proc_self_status_vnode.fs_data = (void*)(uintptr_t)-1;
    proc_self_status_vnode.ops = &status_ops;
    proc_self_status_vnode.refcount = 1000;
    proc_self_status_vnode.parent = &proc_self_dir_vnode;

    proc_self_cmdline_vnode.type = VNODE_FILE;
    proc_self_cmdline_vnode.size = 0;
    proc_self_cmdline_vnode.fs_data = (void*)(uintptr_t)-1;
    proc_self_cmdline_vnode.ops = &cmdline_ops;
    proc_self_cmdline_vnode.refcount = 1000;
    proc_self_cmdline_vnode.parent = &proc_self_dir_vnode;

    proc_self_fd_dir_vnode.type = VNODE_DIR;
    proc_self_fd_dir_vnode.size = 0;
    proc_self_fd_dir_vnode.fs_data = (void*)(uintptr_t)-1;
    proc_self_fd_dir_vnode.ops = &proc_fd_dir_ops;
    proc_self_fd_dir_vnode.refcount = 1000;
    proc_self_fd_dir_vnode.parent = &proc_self_dir_vnode;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_dir_vnodes[i].type = VNODE_DIR;
        proc_dir_vnodes[i].size = 0;
        proc_dir_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_dir_vnodes[i].ops = &proc_pid_dir_ops;
        proc_dir_vnodes[i].refcount = 1000;
        proc_dir_vnodes[i].parent = &proc_root_vnode;

        proc_status_vnodes[i].type = VNODE_FILE;
        proc_status_vnodes[i].size = 0;
        proc_status_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_status_vnodes[i].ops = &status_ops;
        proc_status_vnodes[i].refcount = 1000;
        proc_status_vnodes[i].parent = &proc_dir_vnodes[i];

        proc_cmdline_vnodes[i].type = VNODE_FILE;
        proc_cmdline_vnodes[i].size = 0;
        proc_cmdline_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_cmdline_vnodes[i].ops = &cmdline_ops;
        proc_cmdline_vnodes[i].refcount = 1000;
        proc_cmdline_vnodes[i].parent = &proc_dir_vnodes[i];

        proc_fd_dir_vnodes[i].type = VNODE_DIR;
        proc_fd_dir_vnodes[i].size = 0;
        proc_fd_dir_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_fd_dir_vnodes[i].ops = &proc_fd_dir_ops;
        proc_fd_dir_vnodes[i].refcount = 1000;
        proc_fd_dir_vnodes[i].parent = &proc_dir_vnodes[i];
    }

    proc_agents_dir_vnode.type = VNODE_DIR;
    proc_agents_dir_vnode.size = 0;
    proc_agents_dir_vnode.fs_data = 0;
    proc_agents_dir_vnode.ops = &proc_agents_dir_ops;
    proc_agents_dir_vnode.refcount = 1000;
    proc_agents_dir_vnode.parent = &proc_root_vnode;

    proc_agents_log_vnode.type = VNODE_FILE;
    proc_agents_log_vnode.size = 0;
    proc_agents_log_vnode.fs_data = 0;
    proc_agents_log_vnode.ops = &agents_log_ops;
    proc_agents_log_vnode.refcount = 1000;
    proc_agents_log_vnode.parent = &proc_agents_dir_vnode;

    for (int i = 0; i < MAX_AGENTS; i++) {
        proc_agent_dir_vnodes[i].type = VNODE_DIR;
        proc_agent_dir_vnodes[i].size = 0;
        proc_agent_dir_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_agent_dir_vnodes[i].ops = &proc_agent_dir_ops;
        proc_agent_dir_vnodes[i].refcount = 1000;
        proc_agent_dir_vnodes[i].parent = &proc_agents_dir_vnode;

        proc_agent_status_vnodes[i].type = VNODE_FILE;
        proc_agent_status_vnodes[i].size = 0;
        proc_agent_status_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_agent_status_vnodes[i].ops = &agent_status_ops;
        proc_agent_status_vnodes[i].refcount = 1000;
        proc_agent_status_vnodes[i].parent = &proc_agent_dir_vnodes[i];

        proc_agent_caps_vnodes[i].type = VNODE_FILE;
        proc_agent_caps_vnodes[i].size = 0;
        proc_agent_caps_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_agent_caps_vnodes[i].ops = &agent_caps_ops;
        proc_agent_caps_vnodes[i].refcount = 1000;
        proc_agent_caps_vnodes[i].parent = &proc_agent_dir_vnodes[i];

        proc_agent_budget_vnodes[i].type = VNODE_FILE;
        proc_agent_budget_vnodes[i].size = 0;
        proc_agent_budget_vnodes[i].fs_data = (void*)(uintptr_t)i;
        proc_agent_budget_vnodes[i].ops = &agent_budget_ops;
        proc_agent_budget_vnodes[i].refcount = 1000;
        proc_agent_budget_vnodes[i].parent = &proc_agent_dir_vnodes[i];
    }
}

vnode_t* procfs_get_root(void) {
    return &proc_root_vnode;
}

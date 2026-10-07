#include "procfs.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "pmm.h"
#include "rtc.h"
#include "vma.h"
#include "errno.h"

static vnode_t proc_root_vnode;
static vnode_t proc_meminfo_vnode;
static vnode_t proc_uptime_vnode;
static vnode_t proc_self_dir_vnode;
static vnode_t proc_self_status_vnode;
static vnode_t proc_self_cmdline_vnode;

static vnode_t proc_dir_vnodes[MAX_PROCESSES];
static vnode_t proc_status_vnodes[MAX_PROCESSES];
static vnode_t proc_cmdline_vnodes[MAX_PROCESSES];

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

static const struct fs_ops status_ops = {
    .read = proc_status_read,
    .stat = procfs_file_stat
};

static const struct fs_ops cmdline_ops = {
    .read = proc_cmdline_read,
    .stat = procfs_file_stat
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
    return 0;
}

static const struct fs_ops proc_pid_dir_ops = {
    .lookup = proc_pid_dir_lookup,
    .readdir = proc_pid_dir_readdir,
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
    if (streq(name, "self")) {
        *out = &proc_self_dir_vnode;
        vnode_ref(&proc_self_dir_vnode);
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
        entry->d_ino = 12; entry->d_type = 2; entry->d_size = 0;
        int p = 0; append_str(entry->d_name, &p, sizeof(entry->d_name), "self");
        return 1;
    }

    /* index >= 5 corresponds to active processes */
    uint32_t target_idx = index - 5;
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
    }
}

vnode_t* procfs_get_root(void) {
    return &proc_root_vnode;
}

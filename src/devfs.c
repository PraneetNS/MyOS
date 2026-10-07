#include "devfs.h"
#include "tty.h"
#include "serial.h"
#include "errno.h"
#include "scheduler.h"

#define NUM_DEVICES 5

static const char* dev_names[NUM_DEVICES] = {
    "console",
    "tty",
    "null",
    "zero",
    "serial0"
};

static int console_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    return tty_read(global_tty, (char*) buf, count);
}

static int console_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    return tty_write(global_tty, (const char*) buf, count);
}

static int null_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    (void) buf;
    (void) count;
    return 0; /* EOF */
}

static int null_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    (void) buf;
    return (int) count; /* Swallow bytes */
}

static int zero_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    for (uint32_t i = 0; i < count; i++) {
        buf[i] = 0;
    }
    return (int) count;
}

static int zero_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    (void) buf;
    return (int) count; /* Swallow bytes */
}

static int serial0_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    if (count == 0) return 0;

    while (!serial_received()) {
        scheduler_yield();
    }

    uint32_t n = 0;
    while (n < count && serial_received()) {
        buf[n++] = (uint8_t) serial_read();
    }
    return (int) n;
}

static int serial0_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    for (uint32_t i = 0; i < count; i++) {
        serial_putc(buf[i]);
    }
    return (int) count;
}

static int dev_chr_stat(vnode_t* vn, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static const struct fs_ops console_ops = {
    .read = console_read,
    .write = console_write,
    .stat = dev_chr_stat
};

static const struct fs_ops null_ops = {
    .read = null_read,
    .write = null_write,
    .stat = dev_chr_stat
};

static const struct fs_ops zero_ops = {
    .read = zero_read,
    .write = zero_write,
    .stat = dev_chr_stat
};

static const struct fs_ops serial0_ops = {
    .read = serial0_read,
    .write = serial0_write,
    .stat = dev_chr_stat
};

static vnode_t dev_nodes[NUM_DEVICES];
static vnode_t dev_root_node;

int devfs_is_tty_vnode(vnode_t* vn) {
    if (!vn) return 0;
    return (vn == &dev_nodes[0] || vn == &dev_nodes[1]);
}

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int devfs_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    if (!name || !out) return -EINVAL;
    if (name[0] == '.' && name[1] == '\0') {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        if (dir->parent) {
            *out = dir->parent;
            vnode_ref(dir->parent);
            return 0;
        }
        *out = dir;
        vnode_ref(dir);
        return 0;
    }

    for (int i = 0; i < NUM_DEVICES; i++) {
        if (streq(name, dev_names[i])) {
            *out = &dev_nodes[i];
            vnode_ref(&dev_nodes[i]);
            return 0;
        }
    }
    return -ENOENT;
}

static int devfs_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (!entry) return -EINVAL;

    if (index == 0) {
        entry->d_ino = 1;
        entry->d_type = 2; /* dir */
        entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino = 2;
        entry->d_type = 2; /* dir */
        entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }

    int dev_idx = (int) index - 2;
    if (dev_idx >= 0 && dev_idx < NUM_DEVICES) {
        entry->d_ino = 10 + dev_idx;
        entry->d_type = 1; /* regular/char device */
        entry->d_size = 0;
        int k = 0;
        while (dev_names[dev_idx][k]) {
            entry->d_name[k] = dev_names[dev_idx][k];
            k++;
        }
        entry->d_name[k] = '\0';
        return 1;
    }

    return 0;
}

static int devfs_root_stat(vnode_t* vn, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static const struct fs_ops devfs_root_ops = {
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .stat = devfs_root_stat
};

void devfs_init(void) {
    dev_root_node.type = VNODE_DIR;
    dev_root_node.size = 0;
    dev_root_node.fs_data = 0;
    dev_root_node.ops = &devfs_root_ops;
    dev_root_node.refcount = 1000;
    dev_root_node.parent = 0;

    /* 0: console */
    dev_nodes[0].type = VNODE_CONSOLE;
    dev_nodes[0].size = 0;
    dev_nodes[0].fs_data = global_tty;
    dev_nodes[0].ops = &console_ops;
    dev_nodes[0].refcount = 1000;
    dev_nodes[0].parent = &dev_root_node;

    /* 1: tty */
    dev_nodes[1].type = VNODE_CONSOLE;
    dev_nodes[1].size = 0;
    dev_nodes[1].fs_data = global_tty;
    dev_nodes[1].ops = &console_ops;
    dev_nodes[1].refcount = 1000;
    dev_nodes[1].parent = &dev_root_node;

    /* 2: null */
    dev_nodes[2].type = VNODE_CONSOLE;
    dev_nodes[2].size = 0;
    dev_nodes[2].fs_data = 0;
    dev_nodes[2].ops = &null_ops;
    dev_nodes[2].refcount = 1000;
    dev_nodes[2].parent = &dev_root_node;

    /* 3: zero */
    dev_nodes[3].type = VNODE_CONSOLE;
    dev_nodes[3].size = 0;
    dev_nodes[3].fs_data = 0;
    dev_nodes[3].ops = &zero_ops;
    dev_nodes[3].refcount = 1000;
    dev_nodes[3].parent = &dev_root_node;

    /* 4: serial0 */
    dev_nodes[4].type = VNODE_CONSOLE;
    dev_nodes[4].size = 0;
    dev_nodes[4].fs_data = 0;
    dev_nodes[4].ops = &serial0_ops;
    dev_nodes[4].refcount = 1000;
    dev_nodes[4].parent = &dev_root_node;
}

vnode_t* devfs_get_root(void) {
    return &dev_root_node;
}

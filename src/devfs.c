#include "devfs.h"
#include "tty.h"
#include "pty.h"
#include "serial.h"
#include "errno.h"
#include "scheduler.h"
#include "fb.h"
#include "vma.h"
#include "uaccess.h"
#include "paging.h"
#include "vmm.h"
#include "input.h"
#include "keyboard.h"
#include <string.h>

/* Flat device count (console, tty, null, zero, serial0, fb0, input_dir, mice, mouse0, kbd0, serial1) */
#define NUM_DEVICES 11

vnode_t dev_nodes[NUM_DEVICES];

static const char* dev_names[NUM_DEVICES] = {
    "console",
    "tty",
    "null",
    "zero",
    "serial0",
    "fb0",
    "input",   /* index 6: /dev/input  (directory) */
    "mice",    /* index 7: /dev/input/mice */
    "mouse0",  /* index 8: /dev/input/mouse0 */
    "kbd0",    /* index 9: /dev/input/kbd0 */
    "serial1"  /* index 10: /dev/serial1 */
};

static tty_t* get_effective_tty(vnode_t* vn) {
    if (vn == &dev_nodes[1]) { /* /dev/tty */
        process_t* me = scheduler_current();
        if (me && me->ctty) return (tty_t*) me->ctty;
    }
    return global_tty;
}

static int console_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) offset;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    tty_t* tty = get_effective_tty(vn);
    return tty_read(tty, (char*) buf, count, nonblock);
}

static int console_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) offset;
    tty_t* tty = get_effective_tty(vn);
    return tty_write(tty, (const char*) buf, count);
}

static short console_poll(vnode_t* vn, short events, void** chan_out) {
    tty_t* tty = get_effective_tty(vn);
    return tty_poll(tty, events, chan_out);
}

static int console_ioctl(vnode_t* vn, unsigned long req, void* argp) {
    tty_t* tty = get_effective_tty(vn);
    return tty_ioctl(tty, req, argp);
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

static short null_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    (void) chan_out;
    return (POLLIN | POLLOUT) & events;
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

static short zero_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    (void) chan_out;
    return (POLLIN | POLLOUT) & events;
}

static int serial0_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    if (count == 0) return 0;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;

    if (!serial_received()) {
        if (nonblock) return -EAGAIN;
        while (!serial_received()) {
            if (signal_has_deliverable(scheduler_current())) return -EINTR;
            scheduler_yield();
        }
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

static short serial0_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    (void) chan_out;
    short revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    if (events & POLLIN) {
        if (serial_received()) revents |= POLLIN;
    }
    return revents;
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
    .stat = dev_chr_stat,
    .poll = console_poll,
    .ioctl = console_ioctl
};

static const struct fs_ops null_ops = {
    .read = null_read,
    .write = null_write,
    .stat = dev_chr_stat,
    .poll = null_poll
};

static const struct fs_ops zero_ops = {
    .read = zero_read,
    .write = zero_write,
    .stat = dev_chr_stat,
    .poll = zero_poll
};

static const struct fs_ops serial0_ops = {
    .read = serial0_read,
    .write = serial0_write,
    .stat = dev_chr_stat,
    .poll = serial0_poll
};

static int serial1_dev_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    return serial1_read(buf, count, nonblock);
}

static int serial1_dev_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    return serial1_write(buf, count);
}

static short serial1_dev_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    return serial1_poll(events, chan_out);
}

static const struct fs_ops serial1_ops = {
    .read = serial1_dev_read,
    .write = serial1_dev_write,
    .stat = dev_chr_stat,
    .poll = serial1_dev_poll
};

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t line_length;
    uint32_t reserved[6];
};

struct fb_fix_screeninfo {
    char id[16];
    uint32_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint32_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

static int fb0_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    if (!fb_is_present() || !g_fb_info.virt_addr) return -ENODEV;
    uint32_t smem_len = g_fb_info.pitch * g_fb_info.height;
    if (offset >= smem_len) return 0;
    if (offset + count > smem_len) count = smem_len - offset;
    memcpy(buf, (const uint8_t*)g_fb_info.virt_addr + offset, count);
    return (int) count;
}

static int fb0_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn;
    if (!fb_is_present() || !g_fb_info.virt_addr) return -ENODEV;
    uint32_t smem_len = g_fb_info.pitch * g_fb_info.height;
    if (offset >= smem_len) return 0;
    if (offset + count > smem_len) count = smem_len - offset;
    memcpy((uint8_t*)g_fb_info.virt_addr + offset, buf, count);
    return (int) count;
}

static int fb0_stat(vnode_t* vn, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_size = fb_is_present() ? (g_fb_info.pitch * g_fb_info.height) : 0;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

static int fb0_ioctl(vnode_t* vn, unsigned long req, void* argp) {
    (void) vn;
    if (!fb_is_present()) return -ENODEV;
    if (!argp) return -EFAULT;

    switch (req) {
        case FBIOGET_VSCREENINFO: {
            struct fb_var_screeninfo vi;
            memset(&vi, 0, sizeof(vi));
            vi.xres = g_fb_info.width;
            vi.yres = g_fb_info.height;
            vi.xres_virtual = g_fb_info.width;
            vi.yres_virtual = g_fb_info.height;
            vi.bits_per_pixel = g_fb_info.bpp;
            vi.line_length = g_fb_info.pitch;

            vi.red.offset = g_fb_info.red_pos;
            vi.red.length = g_fb_info.red_mask;
            vi.green.offset = g_fb_info.green_pos;
            vi.green.length = g_fb_info.green_mask;
            vi.blue.offset = g_fb_info.blue_pos;
            vi.blue.length = g_fb_info.blue_mask;

            if (copy_to_user(argp, &vi, sizeof(vi)) != 0) return -EFAULT;
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            struct fb_fix_screeninfo fi;
            memset(&fi, 0, sizeof(fi));
            strncpy(fi.id, "myos_fb0", 16);
            fi.smem_start = g_fb_info.phys_addr;
            fi.smem_len = g_fb_info.pitch * g_fb_info.height;
            fi.line_length = g_fb_info.pitch;

            if (copy_to_user(argp, &fi, sizeof(fi)) != 0) return -EFAULT;
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static int fb0_mmap(vnode_t* vn, vma_t* vma) {
    (void) vn;
    if (!fb_is_present()) return -ENODEV;
    process_t* me = scheduler_current();
    if (!me) return -ESRCH;

    uint32_t smem_len = g_fb_info.pitch * g_fb_info.height;
    if (vma->offset >= smem_len) return -EINVAL;

    uint32_t size = vma->end - vma->start;
    uint32_t num_pages = (size + 4095) / 4096;

    uint32_t cache_bits = 0;
    if (g_fb_info.is_wc) {
        cache_bits = 0x80u; /* PAT WC */
    } else {
        cache_bits = 0x10u; /* PCD Uncached */
    }

    uint32_t pte_flags = PAGE_PRESENT | PAGE_USER | cache_bits;
    if (vma->prot & VMA_PROT_WRITE) pte_flags |= PAGE_WRITE;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page_va = vma->start + i * 4096;
        uint32_t page_pa = g_fb_info.phys_addr + vma->offset + i * 4096;
        if (vmm_map_page(&me->as, page_va, page_pa, pte_flags) != 0) {
            return -ENOMEM;
        }
        asm volatile ("invlpg (%0)" :: "r"(page_va) : "memory");
    }

    return 0;
}

static short fb0_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    (void) chan_out;
    return (POLLIN | POLLOUT) & events;
}

static const struct fs_ops fb0_ops = {
    .read = fb0_read,
    .write = fb0_write,
    .stat = fb0_stat,
    .ioctl = fb0_ioctl,
    .mmap = fb0_mmap,
    .poll = fb0_poll
};

/* ------------------------------------------------------------------ */
/* Global device node arrays (declared early so all fs_ops can see them) */
/* ------------------------------------------------------------------ */

vnode_t dev_root_node;
static vnode_t input_dir_node;

static int streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------ */
/* /dev/input/mice                                                      */
/* ------------------------------------------------------------------ */

static int mice_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn;
    (void) offset;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    return input_read_mice(buf, count, nonblock);
}

static int mice_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) vn; (void) offset; (void) buf;
    return (int) count; /* silently ignore writes like Linux does */
}

static short mice_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    short revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    if (events & POLLIN) {
        if (input_mice_poll()) {
            revents |= POLLIN;
        } else if (chan_out && !*chan_out) {
            *chan_out = &g_mice_channel_token;
        }
    }
    return revents;
}

static int mice_stat(vnode_t* vn, struct stat* st) {
    st->st_dev   = 0;
    st->st_ino   = (uint32_t) vn;
    st->st_mode  = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_size  = 0;
    st->st_blksize = 3;
    st->st_blocks  = 0;
    return 0;
}

static const struct fs_ops mice_ops = {
    .read  = mice_read,
    .write = mice_write,
    .stat  = mice_stat,
    .poll  = mice_poll
};

/* ------------------------------------------------------------------ */
/* /dev/input/mouse0                                                   */
/* ------------------------------------------------------------------ */

static int mouse0_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn; (void) offset;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    return input_read_mouse0(buf, count, nonblock);
}

static short mouse0_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    short revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    if (events & POLLIN) {
        if (input_mouse0_poll()) {
            revents |= POLLIN;
        } else if (chan_out && !*chan_out) {
            *chan_out = &g_mouse0_channel_token;
        }
    }
    return revents;
}

static const struct fs_ops mouse0_ops = {
    .read  = mouse0_read,
    .write = mice_write,
    .stat  = mice_stat,
    .poll  = mouse0_poll
};

/* ------------------------------------------------------------------ */
/* /dev/input/kbd0                                                     */
/* ------------------------------------------------------------------ */

#define KBD_IOC_GETMODS 0x4B60

static int kbd0_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) vn; (void) offset;
    int flags = vfs_get_current_flags();
    int nonblock = (flags & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    return input_read_kbd0(buf, count, nonblock);
}

static short kbd0_poll(vnode_t* vn, short events, void** chan_out) {
    (void) vn;
    short revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    if (events & POLLIN) {
        if (input_kbd0_poll()) {
            revents |= POLLIN;
        } else if (chan_out && !*chan_out) {
            *chan_out = &g_kbd0_channel_token;
        }
    }
    return revents;
}

static int kbd0_ioctl(vnode_t* vn, unsigned long cmd, void* argp) {
    (void) vn;
    if (cmd == KBD_IOC_GETMODS) {
        if (!argp) return -EFAULT;
        uint32_t mods = keyboard_get_modifiers();
        if (copy_to_user(argp, &mods, sizeof(uint32_t)) != 0)
            return -EFAULT;
        return 0;
    }
    return -EINVAL;
}

static const struct fs_ops kbd0_ops = {
    .read  = kbd0_read,
    .write = mice_write,
    .ioctl = kbd0_ioctl,
    .stat  = mice_stat,
    .poll  = kbd0_poll
};

/* ------------------------------------------------------------------ */
/* /dev/input  directory node                                           */
/* ------------------------------------------------------------------ */

static int input_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    (void) dir;
    if (!name || !out) return -EINVAL;
    if (name[0] == '.' && name[1] == '\0') {
        *out = &input_dir_node;
        vnode_ref(&input_dir_node);
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        *out = &dev_root_node;
        vnode_ref(&dev_root_node);
        return 0;
    }
    if (streq(name, "mice")) {
        *out = &dev_nodes[7];
        vnode_ref(&dev_nodes[7]);
        return 0;
    }
    if (streq(name, "mouse0")) {
        *out = &dev_nodes[8];
        vnode_ref(&dev_nodes[8]);
        return 0;
    }
    if (streq(name, "kbd0")) {
        *out = &dev_nodes[9];
        vnode_ref(&dev_nodes[9]);
        return 0;
    }
    return -ENOENT;
}

static int input_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (!entry) return -EINVAL;
    if (index == 0) {
        entry->d_ino  = 1; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        entry->d_ino  = 2; entry->d_type = 2; entry->d_size = 0;
        entry->d_name[0] = '.'; entry->d_name[1] = '.'; entry->d_name[2] = '\0';
        return 1;
    }
    if (index == 2) {
        entry->d_ino  = 20; entry->d_type = 1; entry->d_size = 0;
        entry->d_name[0] = 'm'; entry->d_name[1] = 'i'; entry->d_name[2] = 'c';
        entry->d_name[3] = 'e'; entry->d_name[4] = '\0';
        return 1;
    }
    if (index == 3) {
        entry->d_ino  = 21; entry->d_type = 1; entry->d_size = 0;
        strcpy(entry->d_name, "mouse0");
        return 1;
    }
    if (index == 4) {
        entry->d_ino  = 22; entry->d_type = 1; entry->d_size = 0;
        strcpy(entry->d_name, "kbd0");
        return 1;
    }
    return 0;
}

static int input_dir_stat(vnode_t* vn, struct stat* st) {
    st->st_dev  = 0;
    st->st_ino  = (uint32_t) vn;
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink= 2;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks  = 0;
    return 0;
}

static const struct fs_ops input_dir_ops = {
    .lookup  = input_dir_lookup,
    .readdir = input_dir_readdir,
    .stat    = input_dir_stat
};

static vnode_t ptmx_node;
static vnode_t pts_dir_node;

static int ptmx_stat(vnode_t* vn, struct stat* st) {
    (void) vn;
    if (!st) return -EINVAL;
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static const struct fs_ops ptmx_ops = {
    .stat = ptmx_stat
};

int devfs_is_ptmx_vnode(vnode_t* vn) {
    return (vn == &ptmx_node);
}

static int pts_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    (void) dir;
    if (!name || !out) return -EINVAL;
    if (name[0] == '.' && name[1] == '\0') {
        *out = dir;
        vnode_ref(dir);
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        *out = dir->parent ? dir->parent : dir;
        vnode_ref(*out);
        return 0;
    }

    int idx = 0;
    int digits = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return -ENOENT;
        idx = idx * 10 + (name[i] - '0');
        digits++;
        if (idx >= MAX_PTYS) return -ENOENT;
    }
    if (digits == 0) return -ENOENT;

    vnode_t* slave_vn = pty_get_slave_vnode(idx);
    if (!slave_vn) return -ENOENT;

    slave_vn->parent = dir;
    vnode_ref(dir);
    *out = slave_vn;
    return 0;
}

static int pts_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    (void) dir;
    if (!entry) return -EINVAL;

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

    int active_idx = pty_get_active_index((int) index - 2);
    if (active_idx < 0) return 0;

    entry->d_ino = 100 + active_idx;
    entry->d_type = 1;
    entry->d_size = 0;

    char buf[16];
    int pos = 0;
    int temp = active_idx;
    if (temp == 0) {
        buf[pos++] = '0';
    } else {
        char rev[16];
        int rpos = 0;
        while (temp > 0) {
            rev[rpos++] = '0' + (temp % 10);
            temp /= 10;
        }
        while (rpos > 0) buf[pos++] = rev[--rpos];
    }
    buf[pos] = '\0';
    for (int k = 0; k <= pos; k++) {
        entry->d_name[k] = buf[k];
    }
    return 1;
}

static int pts_dir_stat(vnode_t* vn, struct stat* st) {
    if (!st) return -EINVAL;
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static const struct fs_ops pts_dir_ops = {
    .lookup  = pts_dir_lookup,
    .readdir = pts_dir_readdir,
    .stat    = pts_dir_stat
};

int devfs_is_tty_vnode(vnode_t* vn) {
    if (!vn) return 0;
    return (vn == &dev_nodes[0] || vn == &dev_nodes[1] || pty_is_ptmx_vnode(vn) || pty_is_slave_vnode(vn, NULL));
}

static const struct {
    const char* name;
    uint32_t type;
} root_dev_entries[] = {
    { "console", 1 },
    { "tty", 1 },
    { "null", 1 },
    { "zero", 1 },
    { "serial0", 1 },
    { "serial1", 1 },
    { "fb0", 1 },
    { "input", 2 },
    { "ptmx", 1 },
    { "pts", 2 },
    { "tty0", 1 },
    { "tty1", 1 },
    { "tty2", 1 },
    { "tty3", 1 }
};

#define NUM_ROOT_DEV_ENTRIES (sizeof(root_dev_entries) / sizeof(root_dev_entries[0]))

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

    if (streq(name, "input")) {
        *out = &input_dir_node;
        vnode_ref(&input_dir_node);
        return 0;
    }
    if (streq(name, "ptmx")) {
        *out = &ptmx_node;
        vnode_ref(&ptmx_node);
        return 0;
    }
    if (streq(name, "pts")) {
        *out = &pts_dir_node;
        vnode_ref(&pts_dir_node);
        return 0;
    }
    if (streq(name, "tty0") || streq(name, "tty1") || streq(name, "tty2") || streq(name, "tty3")) {
        *out = &dev_nodes[1];
        vnode_ref(&dev_nodes[1]);
        return 0;
    }

    if (streq(name, "serial1")) {
        *out = &dev_nodes[10];
        vnode_ref(&dev_nodes[10]);
        return 0;
    }

    for (int i = 0; i < 6; i++) {
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
    if (dev_idx >= 0 && dev_idx < (int) NUM_ROOT_DEV_ENTRIES) {
        entry->d_ino  = 10 + dev_idx;
        entry->d_type = root_dev_entries[dev_idx].type;
        entry->d_size = 0;
        int k = 0;
        while (root_dev_entries[dev_idx].name[k]) {
            entry->d_name[k] = root_dev_entries[dev_idx].name[k];
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

    /* 5: fb0 */
    dev_nodes[5].type = VNODE_FILE;
    dev_nodes[5].size = fb_is_present() ? (g_fb_info.pitch * g_fb_info.height) : 0;
    dev_nodes[5].fs_data = 0;
    dev_nodes[5].ops = &fb0_ops;
    dev_nodes[5].refcount = 1000;
    dev_nodes[5].parent = &dev_root_node;

    /* 6: input directory (placeholder in flat array; real node = input_dir_node) */
    dev_nodes[6].type     = VNODE_DIR;
    dev_nodes[6].size     = 0;
    dev_nodes[6].fs_data  = 0;
    dev_nodes[6].ops      = &input_dir_ops;
    dev_nodes[6].refcount = 1000;
    dev_nodes[6].parent   = &dev_root_node;

    /* 7: /dev/input/mice */
    dev_nodes[7].type     = VNODE_CONSOLE;
    dev_nodes[7].size     = 0;
    dev_nodes[7].fs_data  = 0;
    dev_nodes[7].ops      = &mice_ops;
    dev_nodes[7].refcount = 1000;
    dev_nodes[7].parent   = &input_dir_node;

    /* 8: /dev/input/mouse0 */
    dev_nodes[8].type     = VNODE_CONSOLE;
    dev_nodes[8].size     = 0;
    dev_nodes[8].fs_data  = 0;
    dev_nodes[8].ops      = &mouse0_ops;
    dev_nodes[8].refcount = 1000;
    dev_nodes[8].parent   = &input_dir_node;

    /* 9: /dev/input/kbd0 */
    dev_nodes[9].type     = VNODE_CONSOLE;
    dev_nodes[9].size     = 0;
    dev_nodes[9].fs_data  = 0;
    dev_nodes[9].ops      = &kbd0_ops;
    dev_nodes[9].refcount = 1000;
    dev_nodes[9].parent   = &input_dir_node;

    /* 10: /dev/serial1 (COM2 raw byte device) */
    dev_nodes[10].type     = VNODE_CONSOLE;
    dev_nodes[10].size     = 0;
    dev_nodes[10].fs_data  = 0;
    dev_nodes[10].ops      = &serial1_ops;
    dev_nodes[10].refcount = 1000;
    dev_nodes[10].parent   = &dev_root_node;

    /* /dev/input directory node */
    input_dir_node.type     = VNODE_DIR;
    input_dir_node.size     = 0;
    input_dir_node.fs_data  = 0;
    input_dir_node.ops      = &input_dir_ops;
    input_dir_node.refcount = 1000;
    input_dir_node.parent   = &dev_root_node;

    /* /dev/ptmx node */
    ptmx_node.type     = VNODE_CONSOLE;
    ptmx_node.size     = 0;
    ptmx_node.fs_data  = 0;
    ptmx_node.ops      = &ptmx_ops;
    ptmx_node.refcount = 1000;
    ptmx_node.parent   = &dev_root_node;

    /* /dev/pts directory node */
    pts_dir_node.type     = VNODE_DIR;
    pts_dir_node.size     = 0;
    pts_dir_node.fs_data  = 0;
    pts_dir_node.ops      = &pts_dir_ops;
    pts_dir_node.refcount = 1000;
    pts_dir_node.parent   = &dev_root_node;
}

vnode_t* devfs_get_root(void) {
    return &dev_root_node;
}

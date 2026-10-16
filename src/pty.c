#include "pty.h"
#include "kheap.h"
#include "vfs.h"
#include "tty.h"
#include "scheduler.h"
#include "signal.h"
#include "errno.h"
#include "uaccess.h"
#include "serial.h"

static pty_t ptys[MAX_PTYS];
static kmutex_t pty_table_lock;

static inline uint32_t irq_save(void) {
    uint32_t flags;
    asm volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    asm volatile ("pushl %0; popfl" :: "r"(flags) : "memory");
}

static void pty_slave_output_char(tty_t* tty, char c);

static int pty_master_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count);
static int pty_master_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count);
static short pty_master_poll(vnode_t* vn, short events, void** chan_out);
static int pty_master_ioctl(vnode_t* vn, unsigned long req, void* argp);
static int pty_master_stat(vnode_t* vn, struct stat* st);
static int pty_master_cleanup(vnode_t* vn);

static int pty_slave_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count);
static int pty_slave_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count);
static short pty_slave_poll(vnode_t* vn, short events, void** chan_out);
static int pty_slave_ioctl(vnode_t* vn, unsigned long req, void* argp);
static int pty_slave_stat(vnode_t* vn, struct stat* st);
static int pty_slave_cleanup(vnode_t* vn);

static const struct fs_ops pty_master_ops = {
    .read     = pty_master_read,
    .write    = pty_master_write,
    .poll     = pty_master_poll,
    .ioctl    = pty_master_ioctl,
    .stat     = pty_master_stat,
    .cleanup  = pty_master_cleanup
};

static const struct fs_ops pty_slave_ops = {
    .read     = pty_slave_read,
    .write    = pty_slave_write,
    .poll     = pty_slave_poll,
    .ioctl    = pty_slave_ioctl,
    .stat     = pty_slave_stat,
    .cleanup  = pty_slave_cleanup
};

void pty_init(void) {
    kmutex_init(&pty_table_lock, "pty_table");
    for (int i = 0; i < MAX_PTYS; i++) {
        ptys[i].in_use = 0;
        ptys[i].index = i;
        kmutex_init(&ptys[i].lock, "pty_lock");
    }
}

static void pty_slave_output_char(tty_t* tty, char c) {
    pty_t* pty = (pty_t*) tty->driver_data;
    if (!pty || !pty->in_use || !pty->master_open) return;

    uint32_t f = irq_save();
    if (pty->master_in.count < PTY_BUF_SIZE) {
        pty->master_in.buffer[pty->master_in.head] = (uint8_t) c;
        pty->master_in.head = (pty->master_in.head + 1) % PTY_BUF_SIZE;
        pty->master_in.count++;
    }
    irq_restore(f);

    scheduler_wake_channel(&pty->master_in.read_wait);
}

int pty_create(vnode_t** out_master_vn) {
    if (!out_master_vn) return -EINVAL;

    kmutex_lock(&pty_table_lock);
    int slot = -1;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (!ptys[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        kmutex_unlock(&pty_table_lock);
        return -ENFILE;
    }

    pty_t* pty = &ptys[slot];
    pty->in_use = 1;
    pty->locked = 1; /* Locked until unlockpt() */
    pty->master_open = 1;
    pty->slave_open_count = 0;

    pty->master_in.head = 0;
    pty->master_in.tail = 0;
    pty->master_in.count = 0;
    pty->master_in.read_wait = 0;
    pty->master_in.write_wait = 0;

    tty_init_struct(&pty->slave_tty);
    pty->slave_tty.driver_data = pty;
    pty->slave_tty.is_pty = 1;
    pty->slave_tty.output_char = pty_slave_output_char;

    vnode_t* vn = vnode_alloc(VNODE_CONSOLE, &pty_master_ops, pty);
    if (!vn) {
        pty->in_use = 0;
        kmutex_unlock(&pty_table_lock);
        return -ENOMEM;
    }

    *out_master_vn = vn;
    kmutex_unlock(&pty_table_lock);
    return 0;
}

vnode_t* pty_get_slave_vnode(int index) {
    if (index < 0 || index >= MAX_PTYS) return NULL;

    kmutex_lock(&pty_table_lock);
    pty_t* pty = &ptys[index];
    if (!pty->in_use || !pty->master_open) {
        kmutex_unlock(&pty_table_lock);
        return NULL;
    }

    vnode_t* vn = vnode_alloc(VNODE_CONSOLE, &pty_slave_ops, pty);
    if (vn) {
        pty->slave_open_count++;
    }
    kmutex_unlock(&pty_table_lock);
    return vn;
}

int pty_is_ptmx_vnode(vnode_t* vn) {
    if (!vn) return 0;
    return vn->ops == &pty_master_ops;
}

int pty_is_slave_vnode(vnode_t* vn, int* out_index) {
    if (!vn || vn->ops != &pty_slave_ops) return 0;
    pty_t* pty = (pty_t*) vn->fs_data;
    if (pty && out_index) *out_index = pty->index;
    return 1;
}

int pty_count_active(void) {
    int count = 0;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (ptys[i].in_use && ptys[i].master_open) count++;
    }
    return count;
}

int pty_get_active_index(int slot) {
    int cur = 0;
    for (int i = 0; i < MAX_PTYS; i++) {
        if (ptys[i].in_use && ptys[i].master_open) {
            if (cur == slot) return i;
            cur++;
        }
    }
    return -1;
}

void pty_master_close(pty_t* pty) {
    if (!pty) return;
    kmutex_lock(&pty->lock);
    pty->master_open = 0;

    if (pty->slave_tty.fg_pgid > 0) {
        sys_kill(-pty->slave_tty.fg_pgid, SIGHUP);
        sys_kill(-pty->slave_tty.fg_pgid, SIGCONT);
    }
    scheduler_wake_channel(&pty->slave_tty.read_wait);

    if (pty->slave_open_count == 0) {
        pty->in_use = 0;
    }
    kmutex_unlock(&pty->lock);
}

void pty_slave_close(pty_t* pty) {
    if (!pty) return;
    kmutex_lock(&pty->lock);
    if (pty->slave_open_count > 0) {
        pty->slave_open_count--;
    }
    scheduler_wake_channel(&pty->master_in.read_wait);

    if (pty->slave_open_count == 0 && !pty->master_open) {
        pty->in_use = 0;
    }
    kmutex_unlock(&pty->lock);
}

/* Master Vnode Operations */

static int pty_master_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) offset;
    if (!vn || !vn->fs_data || !buf) return -EINVAL;
    if (count == 0) return 0;

    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;

    int nonblock = (vfs_get_current_flags() & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;

    while (pty->master_in.count == 0) {
        if (pty->slave_open_count == 0) {
            return 0; /* EOF if all slaves closed */
        }
        if (nonblock) return -EAGAIN;
        if (signal_has_deliverable(scheduler_current())) return -EINTR;
        scheduler_wait_channel(&pty->master_in.read_wait);
    }

    uint32_t n = 0;
    uint32_t f = irq_save();
    while (n < count && pty->master_in.count > 0) {
        buf[n++] = pty->master_in.buffer[pty->master_in.tail];
        pty->master_in.tail = (pty->master_in.tail + 1) % PTY_BUF_SIZE;
        pty->master_in.count--;
    }
    irq_restore(f);

    scheduler_wake_channel(&pty->master_in.write_wait);
    return (int) n;
}

static int pty_master_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) offset;
    if (!vn || !vn->fs_data || (!buf && count > 0)) return -EINVAL;
    if (count == 0) return 0;

    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;
    if (pty->slave_open_count == 0) return -EIO;

    for (uint32_t i = 0; i < count; i++) {
        tty_input_char(&pty->slave_tty, (char) buf[i]);
    }
    return (int) count;
}

static short pty_master_poll(vnode_t* vn, short events, void** chan_out) {
    if (!vn || !vn->fs_data) return POLLNVAL;
    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return POLLERR;

    short revents = 0;
    if (events & POLLOUT) {
        if (pty->slave_open_count > 0) {
            revents |= POLLOUT;
        }
    }
    if (events & POLLIN) {
        if (pty->master_in.count > 0 || pty->slave_open_count == 0) {
            revents |= POLLIN;
        } else if (chan_out && !*chan_out) {
            *chan_out = &pty->master_in.read_wait;
        }
    }
    if (pty->slave_open_count == 0) {
        revents |= POLLHUP;
    }
    return revents;
}

static int pty_master_ioctl(vnode_t* vn, unsigned long req, void* argp) {
    if (!vn || !vn->fs_data) return -EINVAL;
    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;

    switch (req) {
        case TIOCGPTN: {
            if (!argp) return -EFAULT;
            int n = pty->index;
            if (copy_to_user(argp, &n, sizeof(int)) != 0) return -EFAULT;
            return 0;
        }
        case TIOCSPTLCK: {
            if (!argp) return -EFAULT;
            int l = 0;
            if (copy_from_user(&l, argp, sizeof(int)) != 0) return -EFAULT;
            pty->locked = l;
            return 0;
        }
        case TIOCGWINSZ:
        case TIOCSWINSZ:
        case TCGETS:
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TIOCGPGRP:
        case TIOCSPGRP:
            return tty_ioctl(&pty->slave_tty, req, argp);
        default:
            return -EINVAL;
    }
}

static int pty_master_stat(vnode_t* vn, struct stat* st) {
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

static int pty_master_cleanup(vnode_t* vn) {
    if (!vn || !vn->fs_data) return 0;
    pty_master_close((pty_t*) vn->fs_data);
    return 0;
}

/* Slave Vnode Operations */

static int pty_slave_read(vnode_t* vn, uint32_t offset, uint8_t* buf, uint32_t count) {
    (void) offset;
    if (!vn || !vn->fs_data || !buf) return -EINVAL;
    if (count == 0) return 0;

    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;
    if (!pty->master_open) return 0; /* EOF if master closed */

    int nonblock = (vfs_get_current_flags() & (O_NONBLOCK | O_NONBLOCK_LINUX)) ? 1 : 0;
    return tty_read(&pty->slave_tty, (char*) buf, count, nonblock);
}

static int pty_slave_write(vnode_t* vn, uint32_t offset, const uint8_t* buf, uint32_t count) {
    (void) offset;
    if (!vn || !vn->fs_data || (!buf && count > 0)) return -EINVAL;
    if (count == 0) return 0;

    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;
    if (!pty->master_open) {
        process_t* me = scheduler_current();
        if (me) sig_send(me, SIGPIPE);
        return -EPIPE;
    }

    return tty_write(&pty->slave_tty, (const char*) buf, count);
}

static short pty_slave_poll(vnode_t* vn, short events, void** chan_out) {
    if (!vn || !vn->fs_data) return POLLNVAL;
    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return POLLERR;
    if (!pty->master_open) return POLLHUP | POLLERR;

    return tty_poll(&pty->slave_tty, events, chan_out);
}

static int pty_slave_ioctl(vnode_t* vn, unsigned long req, void* argp) {
    if (!vn || !vn->fs_data) return -EINVAL;
    pty_t* pty = (pty_t*) vn->fs_data;
    if (!pty->in_use) return -EIO;

    return tty_ioctl(&pty->slave_tty, req, argp);
}

static int pty_slave_stat(vnode_t* vn, struct stat* st) {
    if (!vn || !vn->fs_data || !st) return -EINVAL;
    st->st_dev = 0;
    st->st_ino = (uint32_t) vn;
    st->st_mode = S_IFCHR | 0620;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_blksize = 512;
    st->st_blocks = 0;
    return 0;
}

static int pty_slave_cleanup(vnode_t* vn) {
    if (!vn || !vn->fs_data) return 0;
    pty_slave_close((pty_t*) vn->fs_data);
    return 0;
}

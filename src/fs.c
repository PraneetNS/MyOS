#include "fs.h"
#include "ata.h"
#include "vga.h"

#define FS_MAGIC 0x4D594653u /* "MYFS" as a little-endian u32 */
#define FS_MAX_ENTRIES 32
#define FS_DIR_START_LBA 1
#define ENTRIES_PER_SECTOR (512 / 64)

/* On-disk layout: must match tools/build_disk.py exactly. */
typedef struct {
    char name[FS_MAX_NAME];  /* 48 bytes */
    uint32_t start_lba;      /* 4 bytes  */
    uint32_t size_bytes;     /* 4 bytes  */
    uint8_t  reserved[8];    /* 8 bytes -- pads the entry to 64 bytes */
} __attribute__((packed)) fs_entry_disk_t;

static fs_entry_t entries[FS_MAX_ENTRIES];
static int entry_count = 0;
static int fs_mounted = 0;

void fs_init(void) {
    uint8_t sector[512];

    if (ata_read_sector(0, sector) != 0) {
        terminal_writestring("[fs] ERROR: failed to read superblock (LBA 0)\n");
        return;
    }

    uint32_t magic = *(uint32_t*)&sector[0];
    uint32_t file_count = *(uint32_t*)&sector[4];

    if (magic != FS_MAGIC) {
        terminal_writestring("[fs] ERROR: bad superblock magic -- is the disk image attached?\n");
        return;
    }
    if (file_count > FS_MAX_ENTRIES) file_count = FS_MAX_ENTRIES;

    int sectors_needed = (file_count + ENTRIES_PER_SECTOR - 1) / ENTRIES_PER_SECTOR;
    int idx = 0;

    for (int s = 0; s < sectors_needed; s++) {
        uint8_t dirbuf[512];
        if (ata_read_sector(FS_DIR_START_LBA + s, dirbuf) != 0) {
            terminal_writestring("[fs] ERROR: failed reading directory table\n");
            return;
        }
        fs_entry_disk_t* disk_entries = (fs_entry_disk_t*) dirbuf;
        for (int i = 0; i < ENTRIES_PER_SECTOR && idx < (int)file_count; i++, idx++) {
            for (int c = 0; c < FS_MAX_NAME; c++)
                entries[idx].name[c] = disk_entries[i].name[c];
            entries[idx].start_lba  = disk_entries[i].start_lba;
            entries[idx].size_bytes = disk_entries[i].size_bytes;
        }
    }

    entry_count = file_count;
    fs_mounted = 1;

    terminal_writestring("[ok] Filesystem mounted, ");
    /* tiny inline decimal printer */
    char buf[11]; int bi = 10; buf[10] = '\0';
    uint32_t n = (uint32_t) entry_count;
    if (n == 0) { terminal_writestring("0"); }
    else { while (n > 0 && bi > 0) { buf[--bi] = '0' + (n % 10); n /= 10; } terminal_writestring(&buf[bi]); }
    terminal_writestring(" file(s) found\n");
}

int fs_list(void) {
    if (!fs_mounted) {
        terminal_writestring("[fs] not mounted\n");
        return 0;
    }
    for (int i = 0; i < entry_count; i++) {
        terminal_writestring("  ");
        terminal_writestring(entries[i].name);
        terminal_writestring("\n");
    }
    return entry_count;
}

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

const fs_entry_t* fs_find(const char* name) {
    if (!fs_mounted) return 0;
    for (int i = 0; i < entry_count; i++)
        if (streq(entries[i].name, name))
            return &entries[i];
    return 0;
}

int fs_read_file(const fs_entry_t* entry, uint8_t* buffer) {
    if (!entry) return -1;

    uint32_t sectors = (entry->size_bytes + 511) / 512;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_read_sector(entry->start_lba + s, buffer + s * 512) != 0)
            return -1;
    }
    return (int) entry->size_bytes;
}

int fs_read_range(const fs_entry_t* entry, uint32_t offset, uint8_t* buffer, uint32_t maxlen) {
    if (!entry) return -1;
    if (offset >= entry->size_bytes) return 0; /* EOF */

    uint32_t remaining_in_file = entry->size_bytes - offset;
    uint32_t to_read = (maxlen < remaining_in_file) ? maxlen : remaining_in_file;

    uint32_t done = 0;
    while (done < to_read) {
        uint32_t file_pos = offset + done;
        uint32_t sector_index = file_pos / 512;
        uint32_t sector_offset = file_pos % 512;

        uint8_t sector[512];
        if (ata_read_sector(entry->start_lba + sector_index, sector) != 0) return -1;

        uint32_t chunk = 512 - sector_offset;
        if (chunk > to_read - done) chunk = to_read - done;
        for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = sector[sector_offset + i];
        done += chunk;
    }
    return (int) done;
}

#include "vfs.h"

static int myfs_read(vnode_t* node, uint32_t offset, uint8_t* buf, uint32_t count) {
    const fs_entry_t* entry = (const fs_entry_t*) node->fs_data;
    return fs_read_range(entry, offset, buf, count);
}

static int myfs_stat(vnode_t* node, struct stat* st) {
    st->st_dev = 0;
    st->st_ino = 100;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = node->size;
    st->st_blksize = 512;
    st->st_blocks = (node->size + 511) / 512;
    return 0;
}

static const struct fs_ops myfs_file_ops = {
    .lookup = 0,
    .read = myfs_read,
    .write = 0,
    .create = 0,
    .mkdir = 0,
    .unlink = 0,
    .rmdir = 0,
    .rename = 0,
    .readdir = 0,
    .truncate = 0,
    .stat = myfs_stat,
};

static int myfs_dir_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    (void) dir;
    const fs_entry_t* e = fs_find(name);
    if (!e) return -ENOENT;

    vnode_t* vn = vnode_alloc(VNODE_FILE, &myfs_file_ops, (void*) e);
    if (!vn) return -ENOMEM;
    vn->size = e->size_bytes;
    *out = vn;
    return 0;
}

static int myfs_dir_readdir(vnode_t* dir, uint32_t index, struct dirent* dent) {
    (void) dir;
    if ((int)index >= entry_count) return 0;
    dent->d_ino = 100 + index;
    dent->d_type = 1;
    dent->d_size = entries[index].size_bytes;
    int j = 0;
    while (entries[index].name[j] && j < 63) {
        dent->d_name[j] = entries[index].name[j];
        j++;
    }
    dent->d_name[j] = '\0';
    return 1;
}

static int myfs_dir_stat(vnode_t* dir, struct stat* st) {
    (void) dir;
    st->st_dev = 0;
    st->st_ino = 1;
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_size = entry_count * 64;
    st->st_blksize = 512;
    st->st_blocks = 1;
    return 0;
}

static const struct fs_ops myfs_root_ops = {
    .lookup = myfs_dir_lookup,
    .read = 0,
    .write = 0,
    .create = 0,
    .mkdir = 0,
    .unlink = 0,
    .rmdir = 0,
    .rename = 0,
    .readdir = myfs_dir_readdir,
    .truncate = 0,
    .stat = myfs_dir_stat,
};

static vnode_t myfs_root_vnode = {
    .type = VNODE_DIR,
    .size = 0,
    .fs_data = 0,
    .ops = &myfs_root_ops,
    .refcount = 1000,
    .parent = 0,
};

vnode_t* myfs_get_root_vnode(void) {
    return &myfs_root_vnode;
}

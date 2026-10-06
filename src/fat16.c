#include "fat16.h"
#include "bcache.h"
#include "kheap.h"
#include "vga.h"
#include "serial.h"

#define FAT_ATTR_READONLY   0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

typedef struct {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed)) fat_bpb_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed)) fat_dirent_t;

typedef struct {
    uint16_t first_cluster;
    uint32_t file_size;
    int is_dir;
    int is_root;

    /* Directory entry location on disk */
    uint32_t dir_sector;
    uint32_t dir_offset;
} fat_node_t;

static fat_bpb_t bpb;
static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sectors;
static uint32_t data_start_lba;
static uint32_t total_data_clusters;

#define FAT_NODE_POOL_SIZE 128
static fat_node_t node_pool[FAT_NODE_POOL_SIZE];
static int node_pool_used[FAT_NODE_POOL_SIZE];

static fat_node_t* alloc_fat_node(void) {
    for (int i = 0; i < FAT_NODE_POOL_SIZE; i++) {
        if (!node_pool_used[i]) {
            node_pool_used[i] = 1;
            node_pool[i].first_cluster = 0;
            node_pool[i].file_size = 0;
            node_pool[i].is_dir = 0;
            node_pool[i].is_root = 0;
            node_pool[i].dir_sector = 0;
            node_pool[i].dir_offset = 0;
            return &node_pool[i];
        }
    }
    return 0;
}

static uint32_t cluster_to_lba(uint16_t cluster) {
    if (cluster < 2) return 0;
    return data_start_lba + (uint32_t)(cluster - 2) * bpb.sectors_per_cluster;
}

static uint16_t fat_get_entry(uint16_t cluster) {
    uint32_t fat_offset = cluster * 2;
    uint32_t sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset = fat_offset % 512;

    uint8_t buf[512];
    if (bcache_read(sector, buf) != 0) return 0xFFFF;
    return *(uint16_t*)&buf[offset];
}

static void fat_set_entry(uint16_t cluster, uint16_t val) {
    uint32_t fat_offset = cluster * 2;
    uint32_t sector = fat_start_lba + (fat_offset / 512);
    uint32_t offset = fat_offset % 512;

    uint8_t buf[512];
    if (bcache_read(sector, buf) == 0) {
        *(uint16_t*)&buf[offset] = val;
        bcache_write(sector, buf);
        for (uint8_t f = 1; f < bpb.fats; f++) {
            bcache_write(sector + f * bpb.sectors_per_fat, buf);
        }
    }
}

static uint16_t fat_alloc_cluster(void) {
    for (uint32_t c = 2; c < total_data_clusters + 2; c++) {
        if (fat_get_entry((uint16_t) c) == 0) {
            uint16_t clus = (uint16_t) c;
            fat_set_entry(clus, 0xFFFF);

            uint32_t lba = cluster_to_lba(clus);
            uint8_t zero[512];
            for (int i = 0; i < 512; i++) zero[i] = 0;
            for (uint32_t s = 0; s < bpb.sectors_per_cluster; s++) {
                bcache_write(lba + s, zero);
            }
            return clus;
        }
    }
    return 0; /* No space */
}

static void fat_free_chain(uint16_t cluster) {
    while (cluster >= 2 && cluster < 0xFFF8) {
        uint16_t next = fat_get_entry(cluster);
        fat_set_entry(cluster, 0);
        cluster = next;
    }
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static void to_dos_name(const char* src, char* dst) {
    for (int i = 0; i < 11; i++) dst[i] = ' ';

    if (src[0] == '.' && src[1] == '\0') {
        dst[0] = '.';
        return;
    }
    if (src[0] == '.' && src[1] == '.' && src[2] == '\0') {
        dst[0] = '.'; dst[1] = '.';
        return;
    }

    int i = 0, j = 0;
    while (src[i] && src[i] != '.' && j < 8) {
        dst[j++] = to_upper(src[i++]);
    }
    while (src[i] && src[i] != '.') i++;
    if (src[i] == '.') {
        i++;
        j = 8;
        while (src[i] && j < 11) {
            dst[j++] = to_upper(src[i++]);
        }
    }
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static void from_dos_name(const char* src, char* dst) {
    int len = 0;
    for (int i = 0; i < 8; i++) {
        if (src[i] == ' ') break;
        dst[len++] = to_lower(src[i]);
    }
    int has_ext = 0;
    for (int i = 8; i < 11; i++) {
        if (src[i] != ' ') { has_ext = 1; break; }
    }
    if (has_ext) {
        dst[len++] = '.';
        for (int i = 8; i < 11; i++) {
            if (src[i] == ' ') break;
            dst[len++] = to_lower(src[i]);
        }
    }
    dst[len] = '\0';
}

static int dos_name_eq(const char* a, const char* b) {
    for (int i = 0; i < 11; i++) {
        if (to_upper(a[i]) != to_upper(b[i])) return 0;
    }
    return 1;
}

static int read_dir_entry(fat_node_t* dir, uint32_t entry_idx, fat_dirent_t* out, uint32_t* out_sec, uint32_t* out_off) {
    uint32_t sector, offset;

    if (dir->is_root) {
        if (entry_idx >= bpb.root_entries) return 0;
        uint32_t byte_off = entry_idx * 32;
        sector = root_dir_start_lba + (byte_off / 512);
        offset = byte_off % 512;
    } else {
        uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
        uint32_t entries_per_cluster = bytes_per_cluster / 32;
        uint32_t cluster_idx = entry_idx / entries_per_cluster;
        uint32_t entry_in_cluster = entry_idx % entries_per_cluster;

        uint16_t curr = dir->first_cluster;
        for (uint32_t i = 0; i < cluster_idx; i++) {
            if (curr < 2 || curr >= 0xFFF8) return 0;
            curr = fat_get_entry(curr);
        }
        if (curr < 2 || curr >= 0xFFF8) return 0;

        uint32_t byte_off = entry_in_cluster * 32;
        sector = cluster_to_lba(curr) + (byte_off / 512);
        offset = byte_off % 512;
    }

    uint8_t buf[512];
    if (bcache_read(sector, buf) != 0) return 0;
    *out = *(fat_dirent_t*)&buf[offset];
    if (out_sec) *out_sec = sector;
    if (out_off) *out_off = offset;
    return 1;
}

static int write_dir_entry(uint32_t sector, uint32_t offset, const fat_dirent_t* in) {
    uint8_t buf[512];
    if (bcache_read(sector, buf) != 0) return -1;
    *(fat_dirent_t*)&buf[offset] = *in;
    return bcache_write(sector, buf);
}

static int find_entry_in_dir(fat_node_t* dir, const char* name_83, fat_dirent_t* out, uint32_t* out_sec, uint32_t* out_off) {
    uint32_t idx = 0;
    fat_dirent_t e;
    uint32_t sec, off;

    while (read_dir_entry(dir, idx++, &e, &sec, &off)) {
        if ((uint8_t)e.name[0] == 0x00) break; /* End of directory */
        if ((uint8_t)e.name[0] == 0xE5) continue; /* Deleted entry */
        if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue; /* Skip LFN */
        if (e.attr & FAT_ATTR_VOLUME_ID) continue; /* Skip volume ID */

        if (dos_name_eq(e.name, name_83)) {
            if (out) *out = e;
            if (out_sec) *out_sec = sec;
            if (out_off) *out_off = off;
            return 1;
        }
    }
    return 0;
}

static int alloc_entry_in_dir(fat_node_t* dir, uint32_t* out_sec, uint32_t* out_off) {
    uint32_t idx = 0;
    fat_dirent_t e;
    uint32_t sec, off;

    while (read_dir_entry(dir, idx++, &e, &sec, &off)) {
        if ((uint8_t)e.name[0] == 0x00 || (uint8_t)e.name[0] == 0xE5) {
            *out_sec = sec;
            *out_off = off;
            return 0;
        }
    }

    /* If not root, extend subdirectory with a new cluster */
    if (!dir->is_root) {
        uint16_t last_clus = dir->first_cluster;
        while (fat_get_entry(last_clus) >= 2 && fat_get_entry(last_clus) < 0xFFF8) {
            last_clus = fat_get_entry(last_clus);
        }
        uint16_t new_clus = fat_alloc_cluster();
        if (new_clus == 0) return -ENOSPC;
        fat_set_entry(last_clus, new_clus);
        *out_sec = cluster_to_lba(new_clus);
        *out_off = 0;
        return 0;
    }

    return -ENOSPC; /* Root directory full */
}

/* Forward declarations of fs_ops */
static const struct fs_ops fat16_ops;

static int fat16_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    char name_83[11];
    to_dos_name(name, name_83);

    fat_dirent_t e;
    uint32_t sec, off;
    if (!find_entry_in_dir(dnode, name_83, &e, &sec, &off)) {
        return -ENOENT;
    }

    fat_node_t* fnode = alloc_fat_node();
    if (!fnode) return -ENOMEM;

    fnode->first_cluster = e.fst_clus_lo;
    fnode->file_size = e.file_size;
    fnode->is_dir = (e.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    fnode->is_root = 0;
    fnode->dir_sector = sec;
    fnode->dir_offset = off;

    vnode_t* vn = vnode_alloc(fnode->is_dir ? VNODE_DIR : VNODE_FILE, &fat16_ops, fnode);
    if (!vn) return -ENOMEM;
    vn->size = fnode->file_size;
    *out = vn;
    return 0;
}

static int fat16_read(vnode_t* node, uint32_t offset, uint8_t* buf, uint32_t count) {
    fat_node_t* fnode = (fat_node_t*) node->fs_data;
    if (offset >= node->size) return 0;
    if (offset + count > node->size) count = node->size - offset;

    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;
    uint32_t cluster_idx = offset / bytes_per_cluster;
    uint32_t cluster_off = offset % bytes_per_cluster;

    uint16_t curr = fnode->first_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        if (curr < 2 || curr >= 0xFFF8) return 0;
        curr = fat_get_entry(curr);
    }

    uint32_t done = 0;
    while (done < count && curr >= 2 && curr < 0xFFF8) {
        uint32_t lba = cluster_to_lba(curr) + (cluster_off / 512);
        uint32_t sec_off = cluster_off % 512;

        uint8_t sbuf[512];
        if (bcache_read(lba, sbuf) != 0) break;

        uint32_t chunk = 512 - sec_off;
        if (chunk > count - done) chunk = count - done;

        for (uint32_t i = 0; i < chunk; i++) {
            buf[done + i] = sbuf[sec_off + i];
        }

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= bytes_per_cluster) {
            cluster_off = 0;
            curr = fat_get_entry(curr);
        }
    }
    return (int) done;
}

static int fat16_write(vnode_t* node, uint32_t offset, const uint8_t* buf, uint32_t count) {
    fat_node_t* fnode = (fat_node_t*) node->fs_data;
    if (count == 0) return 0;

    uint32_t bytes_per_cluster = bpb.sectors_per_cluster * 512;

    if (fnode->first_cluster == 0) {
        fnode->first_cluster = fat_alloc_cluster();
        if (fnode->first_cluster == 0) return -ENOSPC;
    }

    uint32_t cluster_idx = offset / bytes_per_cluster;
    uint32_t cluster_off = offset % bytes_per_cluster;

    uint16_t curr = fnode->first_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        uint16_t next = fat_get_entry(curr);
        if (next < 2 || next >= 0xFFF8) {
            next = fat_alloc_cluster();
            if (next == 0) return -ENOSPC;
            fat_set_entry(curr, next);
        }
        curr = next;
    }

    uint32_t done = 0;
    while (done < count) {
        uint32_t lba = cluster_to_lba(curr) + (cluster_off / 512);
        uint32_t sec_off = cluster_off % 512;

        uint8_t sbuf[512];
        if (sec_off != 0 || (count - done < 512)) {
            bcache_read(lba, sbuf);
        }

        uint32_t chunk = 512 - sec_off;
        if (chunk > count - done) chunk = count - done;

        for (uint32_t i = 0; i < chunk; i++) {
            sbuf[sec_off + i] = buf[done + i];
        }
        if (bcache_write(lba, sbuf) != 0) break;

        done += chunk;
        cluster_off += chunk;
        if (cluster_off >= bytes_per_cluster) {
            cluster_off = 0;
            uint16_t next = fat_get_entry(curr);
            if (next < 2 || next >= 0xFFF8) {
                if (done < count) {
                    next = fat_alloc_cluster();
                    if (next == 0) break;
                    fat_set_entry(curr, next);
                }
            }
            curr = next;
        }
    }

    if (offset + done > node->size) {
        node->size = offset + done;
        fnode->file_size = node->size;

        if (fnode->dir_sector) {
            fat_dirent_t e;
            uint8_t sbuf[512];
            if (bcache_read(fnode->dir_sector, sbuf) == 0) {
                e = *(fat_dirent_t*)&sbuf[fnode->dir_offset];
                e.file_size = node->size;
                e.fst_clus_lo = fnode->first_cluster;
                *(fat_dirent_t*)&sbuf[fnode->dir_offset] = e;
                bcache_write(fnode->dir_sector, sbuf);
            }
        }
    }

    return (int) done;
}

static int fat16_create(vnode_t* dir, const char* name, vnode_t** out) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    char name_83[11];
    to_dos_name(name, name_83);

    if (find_entry_in_dir(dnode, name_83, 0, 0, 0)) {
        return -EEXIST;
    }

    uint32_t sec, off;
    int err = alloc_entry_in_dir(dnode, &sec, &off);
    if (err != 0) return err;

    fat_dirent_t e;
    for (int i = 0; i < 11; i++) e.name[i] = name_83[i];
    e.attr = FAT_ATTR_ARCHIVE;
    e.nt_res = 0;
    e.crt_time_tenth = 0;
    e.crt_time = 0;
    e.crt_date = 0;
    e.lst_acc_date = 0;
    e.fst_clus_hi = 0;
    e.wrt_time = 0;
    e.wrt_date = 0;
    e.fst_clus_lo = 0;
    e.file_size = 0;

    write_dir_entry(sec, off, &e);

    fat_node_t* fnode = alloc_fat_node();
    if (!fnode) return -ENOMEM;
    fnode->first_cluster = 0;
    fnode->file_size = 0;
    fnode->is_dir = 0;
    fnode->is_root = 0;
    fnode->dir_sector = sec;
    fnode->dir_offset = off;

    vnode_t* vn = vnode_alloc(VNODE_FILE, &fat16_ops, fnode);
    if (!vn) return -ENOMEM;
    vn->size = 0;
    *out = vn;
    return 0;
}

static int fat16_mkdir(vnode_t* dir, const char* name, vnode_t** out) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    char name_83[11];
    to_dos_name(name, name_83);

    if (find_entry_in_dir(dnode, name_83, 0, 0, 0)) {
        return -EEXIST;
    }

    uint16_t new_clus = fat_alloc_cluster();
    if (new_clus == 0) return -ENOSPC;

    /* Initialize "." and ".." in new directory cluster */
    uint32_t clus_lba = cluster_to_lba(new_clus);
    uint8_t sbuf[512];
    for (int i = 0; i < 512; i++) sbuf[i] = 0;

    fat_dirent_t* dot = (fat_dirent_t*)&sbuf[0];
    to_dos_name(".", dot->name);
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->fst_clus_lo = new_clus;

    fat_dirent_t* dotdot = (fat_dirent_t*)&sbuf[32];
    to_dos_name("..", dotdot->name);
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->fst_clus_lo = dnode->is_root ? 0 : dnode->first_cluster;

    bcache_write(clus_lba, sbuf);

    uint32_t sec, off;
    int err = alloc_entry_in_dir(dnode, &sec, &off);
    if (err != 0) return err;

    fat_dirent_t e;
    for (int i = 0; i < 11; i++) e.name[i] = name_83[i];
    e.attr = FAT_ATTR_DIRECTORY;
    e.nt_res = 0;
    e.crt_time_tenth = 0;
    e.crt_time = 0;
    e.crt_date = 0;
    e.lst_acc_date = 0;
    e.fst_clus_hi = 0;
    e.wrt_time = 0;
    e.wrt_date = 0;
    e.fst_clus_lo = new_clus;
    e.file_size = 0;

    write_dir_entry(sec, off, &e);

    fat_node_t* fnode = alloc_fat_node();
    if (!fnode) return -ENOMEM;
    fnode->first_cluster = new_clus;
    fnode->file_size = 0;
    fnode->is_dir = 1;
    fnode->is_root = 0;
    fnode->dir_sector = sec;
    fnode->dir_offset = off;

    vnode_t* vn = vnode_alloc(VNODE_DIR, &fat16_ops, fnode);
    if (!vn) return -ENOMEM;
    vn->size = 0;
    *out = vn;
    return 0;
}

static int fat16_unlink(vnode_t* dir, const char* name) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    char name_83[11];
    to_dos_name(name, name_83);

    fat_dirent_t e;
    uint32_t sec, off;
    if (!find_entry_in_dir(dnode, name_83, &e, &sec, &off)) {
        return -ENOENT;
    }
    if (e.attr & FAT_ATTR_DIRECTORY) return -EISDIR;

    if (e.fst_clus_lo) fat_free_chain(e.fst_clus_lo);

    e.name[0] = (char)0xE5;
    write_dir_entry(sec, off, &e);
    return 0;
}

static int fat16_rmdir(vnode_t* dir, const char* name) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    char name_83[11];
    to_dos_name(name, name_83);

    fat_dirent_t e;
    uint32_t sec, off;
    if (!find_entry_in_dir(dnode, name_83, &e, &sec, &off)) {
        return -ENOENT;
    }
    if (!(e.attr & FAT_ATTR_DIRECTORY)) return -ENOTDIR;

    /* Check that directory is empty (only "." and "..") */
    fat_node_t sub;
    sub.first_cluster = e.fst_clus_lo;
    sub.is_dir = 1;
    sub.is_root = 0;

    uint32_t idx = 0;
    fat_dirent_t sub_e;
    while (read_dir_entry(&sub, idx++, &sub_e, 0, 0)) {
        if ((uint8_t)sub_e.name[0] == 0x00) break;
        if ((uint8_t)sub_e.name[0] == 0xE5) continue;
        if ((sub_e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;
        if (sub_e.name[0] == '.' && (sub_e.name[1] == ' ' || (sub_e.name[1] == '.' && sub_e.name[2] == ' '))) {
            continue;
        }
        return -ENOTEMPTY;
    }

    if (e.fst_clus_lo) fat_free_chain(e.fst_clus_lo);
    e.name[0] = (char)0xE5;
    write_dir_entry(sec, off, &e);
    return 0;
}

static int fat16_rename(vnode_t* old_dir, const char* old_name, vnode_t* new_dir, const char* new_name) {
    fat_node_t* odnode = (fat_node_t*) old_dir->fs_data;
    fat_node_t* ndnode = (fat_node_t*) new_dir->fs_data;

    char old_83[11], new_83[11];
    to_dos_name(old_name, old_83);
    to_dos_name(new_name, new_83);

    fat_dirent_t old_e;
    uint32_t old_sec, old_off;
    if (!find_entry_in_dir(odnode, old_83, &old_e, &old_sec, &old_off)) {
        return -ENOENT;
    }

    /* If destination exists, remove it */
    fat_dirent_t exist_e;
    uint32_t exist_sec, exist_off;
    if (find_entry_in_dir(ndnode, new_83, &exist_e, &exist_sec, &exist_off)) {
        if (exist_e.attr & FAT_ATTR_DIRECTORY) return -EISDIR;
        if (exist_e.fst_clus_lo) fat_free_chain(exist_e.fst_clus_lo);
        exist_e.name[0] = (char)0xE5;
        write_dir_entry(exist_sec, exist_off, &exist_e);
    }

    uint32_t new_sec, new_off;
    int err = alloc_entry_in_dir(ndnode, &new_sec, &new_off);
    if (err != 0) return err;

    fat_dirent_t new_e = old_e;
    for (int i = 0; i < 11; i++) new_e.name[i] = new_83[i];
    write_dir_entry(new_sec, new_off, &new_e);

    /* Remove old entry */
    old_e.name[0] = (char)0xE5;
    write_dir_entry(old_sec, old_off, &old_e);
    return 0;
}

static int fat16_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    uint32_t cur_valid = 0;
    uint32_t idx = 0;
    fat_dirent_t e;

    while (read_dir_entry(dnode, idx++, &e, 0, 0)) {
        if ((uint8_t)e.name[0] == 0x00) break;
        if ((uint8_t)e.name[0] == 0xE5) continue;
        if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;
        if (e.attr & FAT_ATTR_VOLUME_ID) continue;

        if (cur_valid == index) {
            from_dos_name(e.name, entry->d_name);
            entry->d_type = (e.attr & FAT_ATTR_DIRECTORY) ? 2 : 1;
            entry->d_size = e.file_size;
            entry->d_ino = e.fst_clus_lo ? e.fst_clus_lo : 1;
            return 1;
        }
        cur_valid++;
    }
    return 0;
}

static int fat16_truncate(vnode_t* node, uint32_t length) {
    fat_node_t* fnode = (fat_node_t*) node->fs_data;
    if (length == 0) {
        if (fnode->first_cluster) {
            fat_free_chain(fnode->first_cluster);
            fnode->first_cluster = 0;
        }
        node->size = 0;
        fnode->file_size = 0;

        if (fnode->dir_sector) {
            fat_dirent_t e;
            uint8_t sbuf[512];
            if (bcache_read(fnode->dir_sector, sbuf) == 0) {
                e = *(fat_dirent_t*)&sbuf[fnode->dir_offset];
                e.file_size = 0;
                e.fst_clus_lo = 0;
                *(fat_dirent_t*)&sbuf[fnode->dir_offset] = e;
                bcache_write(fnode->dir_sector, sbuf);
            }
        }
    }
    return 0;
}

static int fat16_stat(vnode_t* node, struct stat* st) {
    fat_node_t* fnode = (fat_node_t*) node->fs_data;
    st->st_dev = 0;
    st->st_ino = fnode->first_cluster ? fnode->first_cluster : 1;
    st->st_mode = (node->type == VNODE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = node->size;
    st->st_blksize = 512;
    st->st_blocks = (node->size + 511) / 512;
    return 0;
}

static const struct fs_ops fat16_ops = {
    .lookup   = fat16_lookup,
    .read     = fat16_read,
    .write    = fat16_write,
    .create   = fat16_create,
    .mkdir    = fat16_mkdir,
    .unlink   = fat16_unlink,
    .rmdir    = fat16_rmdir,
    .rename   = fat16_rename,
    .readdir  = fat16_readdir,
    .truncate = fat16_truncate,
    .stat     = fat16_stat,
};

static vnode_t fat16_root_vnode;
static fat_node_t fat16_root_node;

int fat16_init(void) {
    uint8_t boot_sector[512];
    if (bcache_read(0, boot_sector) != 0) {
        return -1;
    }

    bpb = *(fat_bpb_t*) boot_sector;
    if (bpb.bytes_per_sector != 512 || bpb.sectors_per_cluster == 0 || bpb.reserved_sectors == 0 || bpb.fats == 0) {
        return -1;
    }

    fat_start_lba = bpb.reserved_sectors;
    root_dir_start_lba = fat_start_lba + ((uint32_t) bpb.fats * bpb.sectors_per_fat);
    root_dir_sectors = (((uint32_t) bpb.root_entries * 32) + 511) / 512;
    data_start_lba = root_dir_start_lba + root_dir_sectors;

    uint32_t total_sectors = bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;
    total_data_clusters = (total_sectors - data_start_lba) / bpb.sectors_per_cluster;

    fat16_root_node.first_cluster = 0;
    fat16_root_node.file_size = 0;
    fat16_root_node.is_dir = 1;
    fat16_root_node.is_root = 1;
    fat16_root_node.dir_sector = 0;
    fat16_root_node.dir_offset = 0;

    fat16_root_vnode.type = VNODE_DIR;
    fat16_root_vnode.size = 0;
    fat16_root_vnode.fs_data = &fat16_root_node;
    fat16_root_vnode.ops = &fat16_ops;
    fat16_root_vnode.refcount = 1000;
    fat16_root_vnode.parent = 0;

    return 0;
}

vnode_t* fat16_get_root(void) {
    return &fat16_root_vnode;
}

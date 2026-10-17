#include "fat16.h"
#include "bcache.h"
#include "kheap.h"
#include "vga.h"
#include "serial.h"
#include "rtc.h"
#include "kmutex.h"

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
_Static_assert(sizeof(fat_dirent_t) == 32, "fat_dirent_t must be 32 bytes");

#define FAT_LFN_LAST          0x40
#define FAT_LFN_SEQ_MASK      0x1F
#define FAT_LFN_MAX_ENTRIES   20
#define FAT_LFN_CHARS_PER_ENT 13

typedef struct {
    uint8_t  seq;          /* Sequence number: 1..31 | 0x40 (LAST_LFN) */
    uint16_t name1[5];     /* Characters 1-5 (UTF-16LE) */
    uint8_t  attr;         /* Always 0x0F (FAT_ATTR_LFN = READONLY|HIDDEN|SYSTEM|VOLUME_ID) */
    uint8_t  type;         /* Reserved, always 0x00 */
    uint8_t  checksum;     /* Checksum of the associated 8.3 alias */
    uint16_t name2[6];     /* Characters 6-11 (UTF-16LE) */
    uint16_t fst_clus_lo;  /* Reserved, always 0x0000 */
    uint16_t name3[2];     /* Characters 12-13 (UTF-16LE) */
} __attribute__((packed)) fat_lfn_entry_t;
_Static_assert(sizeof(fat_lfn_entry_t) == 32, "fat_lfn_entry_t must be 32 bytes");

static __attribute__((unused)) uint8_t fat_lfn_checksum(const char short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i]);
    }
    return sum;
}

static __attribute__((unused)) void fat_lfn_extract_chars(const fat_lfn_entry_t* lfn, uint16_t out[13]) {
    for (int i = 0; i < 5; i++) out[i] = lfn->name1[i];
    for (int i = 0; i < 6; i++) out[5 + i] = lfn->name2[i];
    for (int i = 0; i < 2; i++) out[11 + i] = lfn->name3[i];
}

static __attribute__((unused)) void fat_lfn_set_chars(fat_lfn_entry_t* lfn, const uint16_t in[13]) {
    for (int i = 0; i < 5; i++) lfn->name1[i] = in[i];
    for (int i = 0; i < 6; i++) lfn->name2[i] = in[5 + i];
    for (int i = 0; i < 2; i++) lfn->name3[i] = in[11 + i];
}

static __attribute__((unused)) int fat_lfn_ucs2_to_utf8(const uint16_t* u16, int u16_len, char* out, int max_out) {
    int out_len = 0;
    for (int i = 0; i < u16_len; i++) {
        uint16_t ch = u16[i];
        if (ch == 0x0000 || ch == 0xFFFF) break;
        if (ch < 0x80) {
            if (out_len + 1 >= max_out) break;
            out[out_len++] = (char)ch;
        } else if (ch < 0x800) {
            if (out_len + 2 >= max_out) break;
            out[out_len++] = (char)(0xC0 | (ch >> 6));
            out[out_len++] = (char)(0x80 | (ch & 0x3F));
        } else {
            if (out_len + 3 >= max_out) break;
            out[out_len++] = (char)(0xE0 | (ch >> 12));
            out[out_len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            out[out_len++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[out_len] = '\0';
    return out_len;
}

static __attribute__((unused)) int fat_lfn_utf8_to_ucs2(const char* utf8, uint16_t* out, int max_out) {
    int u16_len = 0;
    const uint8_t* p = (const uint8_t*)utf8;
    while (*p && u16_len < max_out) {
        if (*p < 0x80) {
            out[u16_len++] = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            uint16_t c = (*p++ & 0x1F) << 6;
            if (*p) c |= (*p++ & 0x3F);
            out[u16_len++] = c;
        } else if ((*p & 0xF0) == 0xE0) {
            uint16_t c = (*p++ & 0x0F) << 12;
            if (*p) c |= (*p++ & 0x3F) << 6;
            if (*p) c |= (*p++ & 0x3F);
            out[u16_len++] = c;
        } else {
            /* 4-byte surrogate or invalid: replace with '?' */
            p++;
            out[u16_len++] = '?';
        }
    }
    return u16_len;
}


typedef struct {
    uint16_t first_cluster;
    uint32_t file_size;
    int is_dir;
    int is_root;
    int unlinked;

    /* Directory entry location on disk */
    uint32_t dir_sector;
    uint32_t dir_offset;
} fat_node_t;

static kmutex_t fat_mutex;
static void fat_free_chain(uint16_t cluster);

static fat_bpb_t bpb;
static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sectors;
static uint32_t data_start_lba;
static uint32_t total_data_clusters;
static fat_node_t fat16_root_node;

static fat_node_t* alloc_fat_node(void) {
    fat_node_t* n = (fat_node_t*) kmalloc(sizeof(fat_node_t));
    if (!n) return 0;
    n->first_cluster = 0;
    n->file_size = 0;
    n->is_dir = 0;
    n->is_root = 0;
    n->unlinked = 0;
    n->dir_sector = 0;
    n->dir_offset = 0;
    return n;
}

static int fat16_cleanup(vnode_t* node) {
    if (node && node->fs_data && node->fs_data != &fat16_root_node) {
        fat_node_t* fnode = (fat_node_t*) node->fs_data;
        if (fnode->unlinked && fnode->first_cluster) {
            kmutex_lock(&fat_mutex);
            fat_free_chain(fnode->first_cluster);
            fnode->first_cluster = 0;
            kmutex_unlock(&fat_mutex);
        }
        kfree(node->fs_data);
        node->fs_data = 0;
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

static uint16_t last_alloc_cluster = 2;

static uint16_t fat_alloc_cluster(void) {
    uint32_t start = last_alloc_cluster;
    if (start < 2 || start >= total_data_clusters + 2) start = 2;

    for (uint32_t i = 0; i < total_data_clusters; i++) {
        uint32_t c = start + i;
        if (c >= total_data_clusters + 2) {
            c = 2 + (c - (total_data_clusters + 2));
        }
        if (fat_get_entry((uint16_t) c) == 0) {
            uint16_t clus = (uint16_t) c;
            fat_set_entry(clus, 0xFFFF);
            last_alloc_cluster = clus + 1;
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

static int strcasecmp_ascii(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return (int)((unsigned char)ca - (unsigned char)cb);
        a++; b++;
    }
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
    return (int)((unsigned char)ca - (unsigned char)cb);
}

typedef struct {
    uint16_t chars[260];
    uint8_t  checksum;
    int      expected_seq;
    int      is_valid;
    uint32_t start_sec;
    uint32_t start_off;
    uint32_t slots;
} fat_lfn_accum_t;

static void lfn_accum_reset(fat_lfn_accum_t* acc) {
    acc->is_valid = 0;
    acc->expected_seq = 0;
    acc->checksum = 0;
    acc->slots = 0;
    acc->start_sec = 0;
    acc->start_off = 0;
}

static void lfn_accum_feed(fat_lfn_accum_t* acc, const fat_dirent_t* e, uint32_t sec, uint32_t off) {
    if ((e->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) return;
    const fat_lfn_entry_t* lfn = (const fat_lfn_entry_t*) e;
    if (lfn->seq & FAT_LFN_LAST) {
        int seq = lfn->seq & FAT_LFN_SEQ_MASK;
        if (seq > 0 && seq <= FAT_LFN_MAX_ENTRIES) {
            acc->is_valid = 1;
            acc->slots = seq;
            acc->checksum = lfn->checksum;
            acc->expected_seq = seq;
            acc->start_sec = sec;
            acc->start_off = off;
            for (int i = 0; i < seq * FAT_LFN_CHARS_PER_ENT; i++) {
                acc->chars[i] = 0xFFFF;
            }
            fat_lfn_extract_chars(lfn, &acc->chars[(seq - 1) * FAT_LFN_CHARS_PER_ENT]);
            acc->expected_seq--;
        } else {
            lfn_accum_reset(acc);
        }
    } else if (acc->is_valid) {
        int seq = lfn->seq & FAT_LFN_SEQ_MASK;
        if (seq == acc->expected_seq && lfn->checksum == acc->checksum) {
            fat_lfn_extract_chars(lfn, &acc->chars[(seq - 1) * FAT_LFN_CHARS_PER_ENT]);
            acc->expected_seq--;
        } else {
            lfn_accum_reset(acc);
        }
    }
}

static int lfn_accum_get_name(fat_lfn_accum_t* acc, const fat_dirent_t* short_e, char* out_name, int max_name) {
    if (acc->is_valid && acc->expected_seq == 0 && acc->checksum == fat_lfn_checksum(short_e->name)) {
        fat_lfn_ucs2_to_utf8(acc->chars, acc->slots * FAT_LFN_CHARS_PER_ENT, out_name, max_name);
        return 1;
    }
    return 0;
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

static __attribute__((unused)) int fat16_needs_lfn(const char* name) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return 0;
    }
    int dot_count = 0;
    int base_len = 0;
    int ext_len = 0;
    int after_dot = 0;

    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') {
            dot_count++;
            after_dot = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') return 1; /* lowercase requires LFN */
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') {
            return 1; /* spaces or invalid 8.3 chars */
        }
        if (!after_dot) {
            base_len++;
        } else {
            ext_len++;
        }
    }

    if (dot_count > 1 || base_len > 8 || ext_len > 3 || base_len == 0) {
        return 1;
    }
    return 0;
}

static int fat_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static __attribute__((unused)) int fat16_generate_short_alias(fat_node_t* dir, const char* name, char out_83[11]) {
    /* If it doesn't need LFN, standard to_dos_name is sufficient */
    if (!fat16_needs_lfn(name)) {
        to_dos_name(name, out_83);
        return 0;
    }

    char clean_base[16];
    char clean_ext[4];
    int cb_len = 0;
    int ce_len = 0;

    /* Find last dot for extension */
    int last_dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') last_dot = i;
    }

    /* Extract clean base */
    int end_base = (last_dot >= 0) ? last_dot : fat_strlen(name);
    for (int i = 0; i < end_base && cb_len < 8; i++) {
        char c = name[i];
        if (c == '.' || c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') {
            continue;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        clean_base[cb_len++] = c;
    }
    if (cb_len == 0) clean_base[cb_len++] = '_';

    /* Extract clean ext */
    if (last_dot >= 0) {
        for (int i = last_dot + 1; name[i] && ce_len < 3; i++) {
            char c = name[i];
            if (c == '.' || c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') {
                continue;
            }
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            clean_ext[ce_len++] = c;
        }
    }

    /* Try numeric tails ~1 through ~999 */
    for (int num = 1; num <= 999; num++) {
        char tail[8];
        int tlen = 0;
        tail[tlen++] = '~';
        if (num >= 100) {
            tail[tlen++] = (char)('0' + (num / 100));
            tail[tlen++] = (char)('0' + ((num / 10) % 10));
            tail[tlen++] = (char)('0' + (num % 10));
        } else if (num >= 10) {
            tail[tlen++] = (char)('0' + (num / 10));
            tail[tlen++] = (char)('0' + (num % 10));
        } else {
            tail[tlen++] = (char)('0' + num);
        }

        char candidate[11];
        for (int k = 0; k < 11; k++) candidate[k] = ' ';

        int max_base_chars = 8 - tlen;
        int take_base = (cb_len < max_base_chars) ? cb_len : max_base_chars;
        for (int k = 0; k < take_base; k++) candidate[k] = clean_base[k];
        for (int k = 0; k < tlen; k++) candidate[take_base + k] = tail[k];
        for (int k = 0; k < ce_len; k++) candidate[8 + k] = clean_ext[k];

        /* Check collision */
        if (!find_entry_in_dir(dir, candidate, 0, 0, 0)) {
            for (int k = 0; k < 11; k++) out_83[k] = candidate[k];
            return 0;
        }
    }

    return -EEXIST;
}

static int find_entry_in_dir_lfn(fat_node_t* dir, const char* name, fat_dirent_t* out,
                                 uint32_t* out_sec, uint32_t* out_off,
                                 uint32_t* out_short_idx, uint32_t* out_lfn_slots) {
    uint32_t idx = 0;
    fat_dirent_t e;
    uint32_t sec, off;
    fat_lfn_accum_t acc;
    lfn_accum_reset(&acc);

    char name_83[11];
    to_dos_name(name, name_83);

    while (read_dir_entry(dir, idx++, &e, &sec, &off)) {
        if ((uint8_t)e.name[0] == 0x00) break;
        if ((uint8_t)e.name[0] == 0xE5) { lfn_accum_reset(&acc); continue; }
        if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            lfn_accum_feed(&acc, &e, sec, off);
            continue;
        }
        if (e.attr & FAT_ATTR_VOLUME_ID) { lfn_accum_reset(&acc); continue; }

        char lfn_name[NAME_MAX + 1];
        int has_lfn = lfn_accum_get_name(&acc, &e, lfn_name, sizeof(lfn_name));
        uint32_t lfn_slots = acc.slots;

        int match = 0;
        if (has_lfn && strcasecmp_ascii(name, lfn_name) == 0) {
            match = 1;
        } else if (dos_name_eq(e.name, name_83)) {
            match = 1;
        }

        if (match) {
            if (out) *out = e;
            if (out_sec) *out_sec = sec;
            if (out_off) *out_off = off;
            if (out_short_idx) *out_short_idx = idx - 1;
            if (out_lfn_slots) *out_lfn_slots = has_lfn ? lfn_slots : 0;
            return 1;
        }
        lfn_accum_reset(&acc);
    }
    return 0;
}

static int delete_entry_and_lfn(fat_node_t* dir, uint32_t short_idx, int lfn_slots) {
    for (int i = 0; i <= lfn_slots; i++) {
        uint32_t target_idx = short_idx - (lfn_slots - i);
        fat_dirent_t de;
        uint32_t s, o;
        if (read_dir_entry(dir, target_idx, &de, &s, &o)) {
            de.name[0] = (char)0xE5;
            write_dir_entry(s, o, &de);
        }
    }
    return 0;
}

static __attribute__((unused)) int alloc_entry_in_dir(fat_node_t* dir, uint32_t* out_sec, uint32_t* out_off) {
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

        uint32_t clba = cluster_to_lba(new_clus);
        uint8_t zbuf[512];
        for (int i = 0; i < 512; i++) zbuf[i] = 0;
        for (uint32_t s = 0; s < bpb.sectors_per_cluster; s++) {
            bcache_write(clba + s, zbuf);
        }

        *out_sec = clba;
        *out_off = 0;
        return 0;
    }

    return -ENOSPC; /* Root directory full */
}

static int write_dir_entry_at_idx(fat_node_t* dir, uint32_t idx, const void* in_32) {
    fat_dirent_t dummy;
    uint32_t sec, off;
    if (!read_dir_entry(dir, idx, &dummy, &sec, &off)) {
        return -EIO;
    }
    return write_dir_entry(sec, off, (const fat_dirent_t*)in_32);
}

static int alloc_contiguous_entries_in_dir(fat_node_t* dir, int count, uint32_t* out_start_idx) {
    uint32_t idx = 0;
    fat_dirent_t e;
    uint32_t sec, off;
    int run = 0;
    uint32_t run_start = 0;

    for (;;) {
        if (!read_dir_entry(dir, idx, &e, &sec, &off)) {
            if (dir->is_root) {
                return -ENOSPC;
            }
            uint16_t last_clus = dir->first_cluster;
            while (fat_get_entry(last_clus) >= 2 && fat_get_entry(last_clus) < 0xFFF8) {
                last_clus = fat_get_entry(last_clus);
            }
            uint16_t new_clus = fat_alloc_cluster();
            if (new_clus == 0) return -ENOSPC;
            fat_set_entry(last_clus, new_clus);

            uint32_t clba = cluster_to_lba(new_clus);
            uint8_t zbuf[512];
            for (int i = 0; i < 512; i++) zbuf[i] = 0;
            for (uint32_t s = 0; s < bpb.sectors_per_cluster; s++) {
                bcache_write(clba + s, zbuf);
            }
            continue;
        }

        if ((uint8_t)e.name[0] == 0x00 || (uint8_t)e.name[0] == 0xE5) {
            if (run == 0) {
                run_start = idx;
            }
            run++;
            if (run == count) {
                *out_start_idx = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
        idx++;
    }
}

static int write_lfn_set(fat_node_t* dir, const char* name, const char short_83[11],
                          const fat_dirent_t* short_e, uint32_t* out_sec, uint32_t* out_off) {
    int needs_lfn = fat16_needs_lfn(name);
    int num_lfn = 0;
    uint16_t u16_name[260];
    int u16_len = 0;

    if (needs_lfn) {
        u16_len = fat_lfn_utf8_to_ucs2(name, u16_name, 256);
        num_lfn = (u16_len + FAT_LFN_CHARS_PER_ENT - 1) / FAT_LFN_CHARS_PER_ENT;
    }
    int total_slots = num_lfn + 1;

    uint32_t start_idx = 0;
    int err = alloc_contiguous_entries_in_dir(dir, total_slots, &start_idx);
    if (err != 0) return err;

    uint8_t chk = fat_lfn_checksum(short_83);

    for (int seq = num_lfn; seq >= 1; seq--) {
        fat_lfn_entry_t lfn;
        lfn.seq = (uint8_t)seq;
        if (seq == num_lfn) lfn.seq |= FAT_LFN_LAST;
        lfn.attr = FAT_ATTR_LFN;
        lfn.type = 0;
        lfn.checksum = chk;
        lfn.fst_clus_lo = 0;

        uint16_t chars[13];
        int char_start = (seq - 1) * FAT_LFN_CHARS_PER_ENT;
        int nul_placed = 0;
        for (int i = 0; i < 13; i++) {
            int char_pos = char_start + i;
            if (char_pos < u16_len) {
                chars[i] = u16_name[char_pos];
            } else if (!nul_placed) {
                chars[i] = 0x0000;
                nul_placed = 1;
            } else {
                chars[i] = 0xFFFF;
            }
        }
        fat_lfn_set_chars(&lfn, chars);
        uint32_t slot_idx = start_idx + (num_lfn - seq);
        write_dir_entry_at_idx(dir, slot_idx, &lfn);
    }

    uint32_t short_slot_idx = start_idx + num_lfn;
    fat_dirent_t dummy;
    uint32_t sec, off;
    read_dir_entry(dir, short_slot_idx, &dummy, &sec, &off);
    write_dir_entry(sec, off, short_e);

    if (out_sec) *out_sec = sec;
    if (out_off) *out_off = off;
    return 0;
}

/* Forward declarations of fs_ops */
static const struct fs_ops fat16_ops;

static int fat16_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    fat_dirent_t e;
    uint32_t sec, off;

    if (!find_entry_in_dir_lfn(dnode, name, &e, &sec, &off, 0, 0)) {
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
    if (!vn) {
        kfree(fnode);
        return -ENOMEM;
    }
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

        uint32_t chunk = 512 - sec_off;
        if (chunk > count - done) chunk = count - done;

        if (sec_off == 0 && chunk == 512) {
            if (bcache_write(lba, buf + done) != 0) break;
        } else {
            uint8_t sbuf[512];
            bcache_read(lba, sbuf);
            for (uint32_t i = 0; i < chunk; i++) {
                sbuf[sec_off + i] = buf[done + i];
            }
            if (bcache_write(lba, sbuf) != 0) break;
        }

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

    if (done > 0 && fnode->dir_sector) {
        fat_dirent_t e;
        uint8_t sbuf[512];
        if (bcache_read(fnode->dir_sector, sbuf) == 0) {
            e = *(fat_dirent_t*)&sbuf[fnode->dir_offset];
            if (offset + done > node->size) {
                node->size = offset + done;
                fnode->file_size = node->size;
                e.file_size = node->size;
                e.fst_clus_lo = fnode->first_cluster;
            }
            uint16_t cur_date, cur_time;
            epoch_to_fat_datetime(rtc_get_epoch(), &cur_date, &cur_time);
            e.wrt_date = cur_date;
            e.wrt_time = cur_time;
            *(fat_dirent_t*)&sbuf[fnode->dir_offset] = e;
            bcache_write(fnode->dir_sector, sbuf);
        }
    } else if (offset + done > node->size) {
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

    if (find_entry_in_dir_lfn(dnode, name, 0, 0, 0, 0, 0)) {
        return -EEXIST;
    }

    char short_83[11];
    int err = fat16_generate_short_alias(dnode, name, short_83);
    if (err != 0) return err;

    fat_dirent_t e;
    for (int i = 0; i < 11; i++) e.name[i] = short_83[i];
    e.attr = FAT_ATTR_ARCHIVE;
    e.nt_res = 0;
    uint16_t cur_date, cur_time;
    epoch_to_fat_datetime(rtc_get_epoch(), &cur_date, &cur_time);
    e.crt_time_tenth = 0;
    e.crt_time = cur_time;
    e.crt_date = cur_date;
    e.lst_acc_date = cur_date;
    e.fst_clus_hi = 0;
    e.wrt_time = cur_time;
    e.wrt_date = cur_date;
    e.fst_clus_lo = 0;
    e.file_size = 0;

    uint32_t sec, off;
    err = write_lfn_set(dnode, name, short_83, &e, &sec, &off);
    if (err != 0) return err;

    fat_node_t* fnode = alloc_fat_node();
    if (!fnode) return -ENOMEM;
    fnode->first_cluster = 0;
    fnode->file_size = 0;
    fnode->is_dir = 0;
    fnode->is_root = 0;
    fnode->dir_sector = sec;
    fnode->dir_offset = off;

    vnode_t* vn = vnode_alloc(VNODE_FILE, &fat16_ops, fnode);
    if (!vn) {
        kfree(fnode);
        return -ENOMEM;
    }
    vn->size = 0;
    *out = vn;
    return 0;
}

static int fat16_mkdir(vnode_t* dir, const char* name, vnode_t** out) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;

    if (find_entry_in_dir_lfn(dnode, name, 0, 0, 0, 0, 0)) {
        return -EEXIST;
    }

    char short_83[11];
    int err = fat16_generate_short_alias(dnode, name, short_83);
    if (err != 0) return err;

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
    for (int i = 0; i < 512; i++) sbuf[i] = 0;
    for (uint32_t s = 1; s < bpb.sectors_per_cluster; s++) {
        bcache_write(clus_lba + s, sbuf);
    }

    fat_dirent_t e;
    for (int i = 0; i < 11; i++) e.name[i] = short_83[i];
    e.attr = FAT_ATTR_DIRECTORY;
    e.nt_res = 0;
    uint16_t cur_date, cur_time;
    epoch_to_fat_datetime(rtc_get_epoch(), &cur_date, &cur_time);
    e.crt_time_tenth = 0;
    e.crt_time = cur_time;
    e.crt_date = cur_date;
    e.lst_acc_date = cur_date;
    e.fst_clus_hi = 0;
    e.wrt_time = cur_time;
    e.wrt_date = cur_date;
    e.fst_clus_lo = new_clus;
    e.file_size = 0;

    uint32_t sec, off;
    err = write_lfn_set(dnode, name, short_83, &e, &sec, &off);
    if (err != 0) {
        fat_free_chain(new_clus);
        return err;
    }

    fat_node_t* fnode = alloc_fat_node();
    if (!fnode) return -ENOMEM;
    fnode->first_cluster = new_clus;
    fnode->file_size = 0;
    fnode->is_dir = 1;
    fnode->is_root = 0;
    fnode->dir_sector = sec;
    fnode->dir_offset = off;

    vnode_t* vn = vnode_alloc(VNODE_DIR, &fat16_ops, fnode);
    if (!vn) {
        kfree(fnode);
        return -ENOMEM;
    }
    vn->size = 0;
    *out = vn;
    return 0;
}

struct fat_unlink_match {
    uint32_t sec;
    uint32_t off;
    int found_open;
};

static void fat_unlink_cb(vnode_t* vn, void* arg) {
    struct fat_unlink_match* m = (struct fat_unlink_match*) arg;
    if (vn->ops == &fat16_ops) {
        fat_node_t* fn = (fat_node_t*) vn->fs_data;
        if (fn && fn->dir_sector == m->sec && fn->dir_offset == m->off) {
            fn->unlinked = 1;
            m->found_open = 1;
        }
    }
}

static int fat16_unlink(vnode_t* dir, const char* name) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;

    fat_dirent_t e;
    uint32_t sec, off, short_idx, lfn_slots;
    if (!find_entry_in_dir_lfn(dnode, name, &e, &sec, &off, &short_idx, &lfn_slots)) {
        return -ENOENT;
    }
    if (e.attr & FAT_ATTR_DIRECTORY) return -EISDIR;

    struct fat_unlink_match m = { sec, off, 0 };
    vfs_for_each_vnode(fat_unlink_cb, &m);

    if (!m.found_open) {
        if (e.fst_clus_lo) fat_free_chain(e.fst_clus_lo);
    }

    delete_entry_and_lfn(dnode, short_idx, (int)lfn_slots);
    return 0;
}

static int fat16_rmdir(vnode_t* dir, const char* name) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;

    fat_dirent_t e;
    uint32_t sec, off, short_idx, lfn_slots;
    if (!find_entry_in_dir_lfn(dnode, name, &e, &sec, &off, &short_idx, &lfn_slots)) {
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

    delete_entry_and_lfn(dnode, short_idx, (int)lfn_slots);
    return 0;
}

static int fat16_rename(vnode_t* old_dir, const char* old_name, vnode_t* new_dir, const char* new_name) {
    fat_node_t* odnode = (fat_node_t*) old_dir->fs_data;
    fat_node_t* ndnode = (fat_node_t*) new_dir->fs_data;

    fat_dirent_t old_e;
    uint32_t old_sec, old_off, old_idx, old_lfn_slots;
    if (!find_entry_in_dir_lfn(odnode, old_name, &old_e, &old_sec, &old_off, &old_idx, &old_lfn_slots)) {
        return -ENOENT;
    }

    int is_same_file = 0;
    if (odnode == ndnode && strcasecmp_ascii(old_name, new_name) == 0) {
        is_same_file = 1;
    }

    if (!is_same_file) {
        fat_dirent_t exist_e;
        uint32_t exist_sec, exist_off, exist_idx, exist_lfn_slots;
        if (find_entry_in_dir_lfn(ndnode, new_name, &exist_e, &exist_sec, &exist_off, &exist_idx, &exist_lfn_slots)) {
            if (exist_e.attr & FAT_ATTR_DIRECTORY) return -EISDIR;
            if (exist_e.fst_clus_lo) fat_free_chain(exist_e.fst_clus_lo);
            delete_entry_and_lfn(ndnode, exist_idx, (int)exist_lfn_slots);
        }
    } else {
        delete_entry_and_lfn(odnode, old_idx, (int)old_lfn_slots);
    }

    char new_83[11];
    int err = fat16_generate_short_alias(ndnode, new_name, new_83);
    if (err != 0) return err;

    fat_dirent_t new_e = old_e;
    for (int i = 0; i < 11; i++) new_e.name[i] = new_83[i];
    uint16_t cur_date, cur_time;
    epoch_to_fat_datetime(rtc_get_epoch(), &cur_date, &cur_time);
    new_e.wrt_date = cur_date;
    new_e.wrt_time = cur_time;

    uint32_t new_sec, new_off;
    err = write_lfn_set(ndnode, new_name, new_83, &new_e, &new_sec, &new_off);
    if (err != 0) return err;

    if (!is_same_file) {
        delete_entry_and_lfn(odnode, old_idx, (int)old_lfn_slots);
    }
    return 0;
}

static int fat16_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    fat_node_t* dnode = (fat_node_t*) dir->fs_data;
    uint32_t cur_valid = 0;
    uint32_t idx = 0;
    fat_dirent_t e;
    uint32_t sec, off;
    fat_lfn_accum_t acc;
    lfn_accum_reset(&acc);

    while (read_dir_entry(dnode, idx++, &e, &sec, &off)) {
        if ((uint8_t)e.name[0] == 0x00) break;
        if ((uint8_t)e.name[0] == 0xE5) { lfn_accum_reset(&acc); continue; }
        if ((e.attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            lfn_accum_feed(&acc, &e, sec, off);
            continue;
        }
        if (e.attr & FAT_ATTR_VOLUME_ID) { lfn_accum_reset(&acc); continue; }

        if (cur_valid == index) {
            char name_buf[NAME_MAX + 1];
            if (!lfn_accum_get_name(&acc, &e, name_buf, sizeof(name_buf))) {
                from_dos_name(e.name, name_buf);
            }
            int l = 0;
            while (name_buf[l] && l < (int)sizeof(entry->d_name) - 1) {
                entry->d_name[l] = name_buf[l];
                l++;
            }
            entry->d_name[l] = '\0';
            entry->d_type = (e.attr & FAT_ATTR_DIRECTORY) ? 2 : 1;
            entry->d_size = e.file_size;
            entry->d_ino = e.fst_clus_lo ? e.fst_clus_lo : 1;
            return 1;
        }
        cur_valid++;
        lfn_accum_reset(&acc);
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

    uint32_t mtime = 0;
    uint32_t ctime = 0;
    if (fnode->dir_sector) {
        uint8_t sbuf[512];
        if (bcache_read(fnode->dir_sector, sbuf) == 0) {
            fat_dirent_t* e = (fat_dirent_t*)&sbuf[fnode->dir_offset];
            mtime = fat_datetime_to_epoch(e->wrt_date, e->wrt_time);
            ctime = fat_datetime_to_epoch(e->crt_date, e->crt_time);
        }
    }
    st->st_mtime = mtime ? mtime : rtc_get_boot_epoch();
    st->st_ctime = ctime ? ctime : st->st_mtime;
    st->st_atime = st->st_mtime;
    return 0;
}

static short fat16_poll(vnode_t* node, short events, void** chan_out) {
    (void) node;
    (void) chan_out;
    return (POLLIN | POLLOUT) & events;
}

static int fat16_locked_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    kmutex_lock(&fat_mutex);
    int r = fat16_lookup(dir, name, out);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_read(vnode_t* node, uint32_t offset, uint8_t* buf, uint32_t count) {
    kmutex_lock(&fat_mutex);
    int r = fat16_read(node, offset, buf, count);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_write(vnode_t* node, uint32_t offset, const uint8_t* buf, uint32_t count) {
    kmutex_lock(&fat_mutex);
    int r = fat16_write(node, offset, buf, count);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_create(vnode_t* dir, const char* name, vnode_t** out) {
    kmutex_lock(&fat_mutex);
    int r = fat16_create(dir, name, out);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_mkdir(vnode_t* dir, const char* name, vnode_t** out) {
    kmutex_lock(&fat_mutex);
    int r = fat16_mkdir(dir, name, out);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_unlink(vnode_t* dir, const char* name) {
    kmutex_lock(&fat_mutex);
    int r = fat16_unlink(dir, name);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_rmdir(vnode_t* dir, const char* name) {
    kmutex_lock(&fat_mutex);
    int r = fat16_rmdir(dir, name);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_rename(vnode_t* old_dir, const char* old_name, vnode_t* new_dir, const char* new_name) {
    kmutex_lock(&fat_mutex);
    int r = fat16_rename(old_dir, old_name, new_dir, new_name);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_readdir(vnode_t* dir, uint32_t index, struct dirent* entry) {
    kmutex_lock(&fat_mutex);
    int r = fat16_readdir(dir, index, entry);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_truncate(vnode_t* node, uint32_t length) {
    kmutex_lock(&fat_mutex);
    int r = fat16_truncate(node, length);
    kmutex_unlock(&fat_mutex);
    return r;
}

static int fat16_locked_stat(vnode_t* node, struct stat* st) {
    kmutex_lock(&fat_mutex);
    int r = fat16_stat(node, st);
    kmutex_unlock(&fat_mutex);
    return r;
}

static const struct fs_ops fat16_ops = {
    .lookup   = fat16_locked_lookup,
    .read     = fat16_locked_read,
    .write    = fat16_locked_write,
    .create   = fat16_locked_create,
    .mkdir    = fat16_locked_mkdir,
    .unlink   = fat16_locked_unlink,
    .rmdir    = fat16_locked_rmdir,
    .rename   = fat16_locked_rename,
    .readdir  = fat16_locked_readdir,
    .truncate = fat16_locked_truncate,
    .stat     = fat16_locked_stat,
    .poll     = fat16_poll,
    .cleanup  = fat16_cleanup
};

static vnode_t fat16_root_vnode;

int fat16_init(void) {
    kmutex_init(&fat_mutex, "fat16");
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

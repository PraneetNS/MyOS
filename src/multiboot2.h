#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

#define MB2_TAG_TYPE_END      0
#define MB2_TAG_TYPE_MMAP     6

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;   /* 1 = available RAM, everything else = reserved/unusable */
    uint32_t reserved;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

#define MB2_MEMORY_AVAILABLE 1

#endif

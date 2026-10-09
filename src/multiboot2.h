#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

#define MB2_TAG_TYPE_END      0
#define MB2_TAG_TYPE_CMDLINE  1
#define MB2_TAG_TYPE_MODULE   3
#define MB2_TAG_TYPE_MMAP     6

struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[];
};

struct mb2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[];
};

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

#define MB2_TAG_TYPE_FRAMEBUFFER 8

#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
    uint8_t  red_field_position;
    uint8_t  red_mask_size;
    uint8_t  green_field_position;
    uint8_t  green_mask_size;
    uint8_t  blue_field_position;
    uint8_t  blue_mask_size;
} __attribute__((packed));

#endif

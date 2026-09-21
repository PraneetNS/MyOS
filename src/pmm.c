#include "pmm.h"
#include "multiboot2.h"
#include "vga.h"

#define FRAME_SIZE 4096
#define MAX_FRAMES (512 * 1024 * 1024 / FRAME_SIZE)  /* bitmap covers up to 512MB of RAM */

static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint32_t highest_frame = 0;   /* highest frame index we know about */
static uint32_t free_frames = 0;

extern uint32_t kernel_end; /* from linker.ld -- symbol address IS the value we want */

static inline void bitmap_set(uint32_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static inline void bitmap_clear(uint32_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static inline int  bitmap_test(uint32_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

static void mark_region_free(uint64_t base, uint64_t len) {
    uint32_t start_frame = (uint32_t)(base / FRAME_SIZE);
    uint32_t frame_count = (uint32_t)(len / FRAME_SIZE);

    for (uint32_t i = 0; i < frame_count; i++) {
        uint32_t frame = start_frame + i;
        if (frame >= MAX_FRAMES) break;
        if (bitmap_test(frame)) {           /* only count frames that were marked used */
            bitmap_clear(frame);
            free_frames++;
        }
        if (frame > highest_frame) highest_frame = frame;
    }
}

static void mark_region_used(uint32_t start_frame, uint32_t frame_count) {
    for (uint32_t i = 0; i < frame_count; i++) {
        uint32_t frame = start_frame + i;
        if (frame >= MAX_FRAMES) break;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (free_frames > 0) free_frames--;
        }
    }
}

void pmm_init(uint32_t mb_info_addr) {
    /* Start with everything marked used; we'll free up what the memory
       map says is actually available RAM. */
    for (uint32_t i = 0; i < MAX_FRAMES / 8; i++)
        frame_bitmap[i] = 0xFF;
    free_frames = 0;

    /* Multiboot2 info: u32 total_size, u32 reserved, then a stream of tags */
    uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;
    uint32_t total_size = *(uint32_t*) ptr;
    uint8_t* end = ptr + total_size;
    uint8_t* tag_ptr = ptr + 8; /* skip the 8-byte fixed header */

    while (tag_ptr < end) {
        struct mb2_tag* tag = (struct mb2_tag*) tag_ptr;
        if (tag->type == MB2_TAG_TYPE_END) break;

        if (tag->type == MB2_TAG_TYPE_MMAP) {
            struct mb2_tag_mmap* mmap = (struct mb2_tag_mmap*) tag;
            uint32_t n_entries = (mmap->size - sizeof(struct mb2_tag_mmap)) / mmap->entry_size;

            for (uint32_t i = 0; i < n_entries; i++) {
                struct mb2_mmap_entry* e =
                    (struct mb2_mmap_entry*)((uint8_t*)mmap->entries + i * mmap->entry_size);
                if (e->type == MB2_MEMORY_AVAILABLE)
                    mark_region_free(e->addr, e->len);
            }
        }

        /* tags are 8-byte aligned */
        tag_ptr += (tag->size + 7) & ~7u;
    }

    /* Reserve the low 1MB (BIOS/real-mode area, video memory) and
       everything the kernel image itself occupies, up to kernel_end. */
    uint32_t reserved_frames = ((uint32_t)&kernel_end + FRAME_SIZE - 1) / FRAME_SIZE;
    mark_region_used(0, reserved_frames);

    terminal_writestring("[ok] Physical memory manager initialized\n");
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            free_frames--;
            return frame * FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_free_frame(uint32_t phys_addr) {
    uint32_t frame = phys_addr / FRAME_SIZE;
    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        free_frames++;
    }
}

uint32_t pmm_free_frame_count(void) {
    return free_frames;
}

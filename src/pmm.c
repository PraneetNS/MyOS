#include "pmm.h"
#include "multiboot2.h"
#include "vga.h"
#include "serial.h"
#include "paging.h"

/* Support up to 16GB of physical RAM in x86-64 long mode:
   16GB / 4096 = 4,194,304 frames.
   Bitmap size: 4,194,304 / 8 = 524,288 bytes (512 KB).
*/
#define MAX_SUPPORTED_RAM_BYTES (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_FRAMES (MAX_SUPPORTED_RAM_BYTES / FRAME_SIZE)

static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint16_t* frame_refcount = 0;
static uint64_t highest_frame = 0;
static uint64_t free_frames = 0;
static uint64_t total_usable_frames = 0;

extern char kernel_end; /* from linker.ld */

static inline void bitmap_set(uint64_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static inline void bitmap_clear(uint64_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static inline int  bitmap_test(uint64_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

static void mark_region_free(uint64_t base, uint64_t len) {
    uint64_t start_frame = base / FRAME_SIZE;
    uint64_t frame_count = len / FRAME_SIZE;

    for (uint64_t i = 0; i < frame_count; i++) {
        uint64_t frame = start_frame + i;
        if (frame >= MAX_FRAMES) break;
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            if (frame_refcount) frame_refcount[frame] = 0;
            free_frames++;
            total_usable_frames++;
        }
        if (frame > highest_frame) highest_frame = frame;
    }
}

static void mark_region_used(uint64_t start_frame, uint64_t frame_count) {
    for (uint64_t i = 0; i < frame_count; i++) {
        uint64_t frame = start_frame + i;
        if (frame >= MAX_FRAMES) break;
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (frame_refcount) frame_refcount[frame] = 1;
            if (free_frames > 0) free_frames--;
        } else {
            if (frame_refcount) frame_refcount[frame] = 1;
        }
    }
}

static void mark_region_used_bytes(uint64_t base, uint64_t len) {
    uint64_t start_frame = base / FRAME_SIZE;
    uint64_t end_frame = (base + len + FRAME_SIZE - 1) / FRAME_SIZE;
    if (end_frame > start_frame) {
        mark_region_used(start_frame, end_frame - start_frame);
    }
}

void pmm_init(uint64_t mb_info_addr) {
    frame_refcount = (void*)0;

    /* Start with everything marked used in bitmap */
    for (uint64_t i = 0; i < MAX_FRAMES / 8; i++)
        frame_bitmap[i] = 0xFF;

    free_frames = 0;
    total_usable_frames = 0;
    highest_frame = 0;

    /* Parse Multiboot2 info: u32 total_size, u32 reserved, then stream of tags */
    uint8_t* ptr = mb_info_addr ? (uint8_t*) P2V(mb_info_addr) : NULL;
    uint32_t total_size = ptr ? *(uint32_t*) ptr : 0;
    uint8_t* end = ptr + total_size;
    uint8_t* tag_ptr = ptr + 8;

    uint64_t max_mod_end = 0;
    while (mb_info_addr && tag_ptr < end) {
        struct mb2_tag* tag = (struct mb2_tag*) tag_ptr;
        if (tag->type == MB2_TAG_TYPE_END) break;

        if (tag->type == MB2_TAG_TYPE_MMAP) {
            struct mb2_tag_mmap* mmap = (struct mb2_tag_mmap*) tag;
            uint32_t n_entries = (mmap->size - sizeof(struct mb2_tag_mmap)) / mmap->entry_size;

            for (uint32_t i = 0; i < n_entries; i++) {
                struct mb2_mmap_entry* e =
                    (struct mb2_mmap_entry*)((uint8_t*)mmap->entries + i * mmap->entry_size);
                if (e->type == MB2_MEMORY_AVAILABLE) {
                    uint64_t addr = e->addr;
                    uint64_t len = e->len;
                    if (addr + len > MAX_SUPPORTED_RAM_BYTES) {
                        if (addr >= MAX_SUPPORTED_RAM_BYTES) continue;
                        len = MAX_SUPPORTED_RAM_BYTES - addr;
                    }
                    mark_region_free(addr, len);
                }
            }
        } else if (tag->type == MB2_TAG_TYPE_MODULE) {
            struct mb2_tag_module* mod = (struct mb2_tag_module*) tag;
            if (mod->mod_end > max_mod_end) {
                max_mod_end = mod->mod_end;
            }
        }

        tag_ptr += (tag->size + 7) & ~7u;
    }

    /* Place frame_refcount dynamically in early physical RAM above the kernel image */
    uint64_t kernel_end_phys = KERN_V2P(&kernel_end);
    uint64_t safe_phys = (kernel_end_phys + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    if (mb_info_addr) {
        uint64_t mb_phys_end = mb_info_addr + total_size;
        if (mb_phys_end > safe_phys) safe_phys = (mb_phys_end + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    }
    if (max_mod_end > safe_phys) {
        safe_phys = (max_mod_end + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    }

    uint64_t max_tracked_frames = highest_frame + 1;
    if (max_tracked_frames > MAX_FRAMES) max_tracked_frames = MAX_FRAMES;
    uint64_t refcount_bytes = max_tracked_frames * sizeof(uint16_t);

    frame_refcount = (uint16_t*) P2V(safe_phys);

    /* Initialize refcounts based on bitmap */
    for (uint64_t i = 0; i < max_tracked_frames; i++) {
        frame_refcount[i] = bitmap_test(i) ? 1 : 0;
    }

    /* 1. Reserve low 1MB (BIOS/real-mode/VGA memory) */
    mark_region_used_bytes(0, 0x100000);

    /* 2. Reserve kernel image + frame_refcount array */
    mark_region_used_bytes(0x100000, (safe_phys + refcount_bytes) - 0x100000);

    /* 3. Reserve Multiboot2 info structure */
    if (mb_info_addr && total_size > 0) {
        uint64_t mb_info_phys = mb_info_addr;
        mark_region_used_bytes(mb_info_phys, total_size);
    }

    /* 4. Reserve Multiboot2 modules */
    tag_ptr = ptr + 8;
    while (mb_info_addr && tag_ptr < end) {
        struct mb2_tag* tag = (struct mb2_tag*) tag_ptr;
        if (tag->type == MB2_TAG_TYPE_END) break;

        if (tag->type == MB2_TAG_TYPE_MODULE) {
            struct mb2_tag_module* mod = (struct mb2_tag_module*) tag;
            if (mod->mod_end > mod->mod_start) {
                mark_region_used_bytes(mod->mod_start, mod->mod_end - mod->mod_start);
            }
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }

    kprintf("[pmm] Total RAM: %u MB (%u frames), Free RAM: %u MB (%u frames)\n",
            (unsigned int)((total_usable_frames * 4) / 1024), (unsigned int)total_usable_frames,
            (unsigned int)((free_frames * 4) / 1024), (unsigned int)free_frames);
}

uint64_t pmm_alloc_frame(void) {
    for (uint64_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (frame_refcount) frame_refcount[frame] = 1;
            free_frames--;
            return frame * FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

/* Allocate a frame strictly below 4GB (0x100000000) for legacy 32-bit DMA (virtio-net, ATA) */
uint64_t pmm_alloc_frame_low(void) {
    uint64_t limit_frame = (4ULL * 1024ULL * 1024ULL * 1024ULL) / FRAME_SIZE;
    if (limit_frame > highest_frame) limit_frame = highest_frame;

    for (uint64_t frame = 0; frame <= limit_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (frame_refcount) frame_refcount[frame] = 1;
            free_frames--;
            return frame * FRAME_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_contiguous_frames(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();
    if (free_frames < count) return 0;

    uint64_t consecutive = 0;
    uint64_t start_frame = 0;

    for (uint64_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) {
            if (consecutive == 0) {
                start_frame = frame;
            }
            consecutive++;
            if (consecutive == count) {
                for (size_t i = 0; i < count; i++) {
                    bitmap_set(start_frame + i);
                    if (frame_refcount) frame_refcount[start_frame + i] = 1;
                }
                free_frames -= count;
                return start_frame * FRAME_SIZE;
            }
        } else {
            consecutive = 0;
        }
    }
    return 0;
}

void pmm_free_contiguous_frames(uint64_t phys_addr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_unref(phys_addr + i * FRAME_SIZE);
    }
}

void pmm_ref(uint64_t phys_addr) {
    uint64_t frame = phys_addr / FRAME_SIZE;
    if (frame < MAX_FRAMES && frame_refcount) {
        frame_refcount[frame]++;
    }
}

void pmm_unref(uint64_t phys_addr) {
    uint64_t frame = phys_addr / FRAME_SIZE;
    if (frame >= MAX_FRAMES || !frame_refcount) return;

    if (frame_refcount[frame] == 0) {
        kprintf("[pmm] ASSERTION FAILED: unref on frame 0x%lx with refcount 0\n", (unsigned long)phys_addr);
        return;
    }

    frame_refcount[frame]--;
    if (frame_refcount[frame] == 0) {
        bitmap_clear(frame);
        free_frames++;
    }
}

void pmm_free_frame(uint64_t phys_addr) {
    pmm_unref(phys_addr);
}

uint16_t pmm_refcount(uint64_t phys_addr) {
    uint64_t frame = phys_addr / FRAME_SIZE;
    if (frame >= MAX_FRAMES || !frame_refcount) return 0;
    return frame_refcount[frame];
}

uint64_t pmm_free_count(void) {
    return free_frames;
}

uint64_t pmm_free_frame_count(void) {
    return free_frames;
}

uint64_t pmm_total_count(void) {
    return total_usable_frames;
}

int pmm_is_ram(uint64_t phys_addr) {
    uint64_t frame = phys_addr / FRAME_SIZE;
    if (frame > highest_frame) return 0;
    return 1;
}

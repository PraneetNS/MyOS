#include "pmm.h"
#include "multiboot2.h"
#include "vga.h"
#include "serial.h"

#define MAX_FRAMES (1024 * 1024 * 1024 / FRAME_SIZE)  /* bitmap covers up to 1GB of RAM (262,144 frames) */

static uint8_t frame_bitmap[MAX_FRAMES / 8];
static uint16_t* frame_refcount = 0;
static uint32_t highest_frame = 0;
static uint32_t free_frames = 0;
static uint32_t total_usable_frames = 0;

extern uint32_t kernel_end; /* from linker.ld */

static inline void bitmap_set(uint32_t frame)   { frame_bitmap[frame / 8] |=  (1 << (frame % 8)); }
static inline void bitmap_clear(uint32_t frame) { frame_bitmap[frame / 8] &= ~(1 << (frame % 8)); }
static inline int  bitmap_test(uint32_t frame)  { return frame_bitmap[frame / 8] & (1 << (frame % 8)); }

static void mark_region_free(uint64_t base, uint64_t len) {
    uint32_t start_frame = (uint32_t)(base / FRAME_SIZE);
    uint32_t frame_count = (uint32_t)(len / FRAME_SIZE);

    for (uint32_t i = 0; i < frame_count; i++) {
        uint32_t frame = start_frame + i;
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

static void mark_region_used(uint32_t start_frame, uint32_t frame_count) {
    for (uint32_t i = 0; i < frame_count; i++) {
        uint32_t frame = start_frame + i;
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
    uint32_t start_frame = (uint32_t)(base / FRAME_SIZE);
    uint32_t end_frame = (uint32_t)((base + len + FRAME_SIZE - 1) / FRAME_SIZE);
    if (end_frame > start_frame) {
        mark_region_used(start_frame, end_frame - start_frame);
    }
}

#include "paging.h"

void pmm_init(uint32_t mb_info_addr) {
    frame_refcount = (void*)0;

    /* Start with everything marked used in bitmap */
    for (uint32_t i = 0; i < MAX_FRAMES / 8; i++)
        frame_bitmap[i] = 0xFF;

    free_frames = 0;
    total_usable_frames = 0;
    highest_frame = 0;

    /* Parse Multiboot2 info: u32 total_size, u32 reserved, then stream of tags */
    uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;
    uint32_t total_size = mb_info_addr ? *(uint32_t*) ptr : 0;
    uint8_t* end = ptr + total_size;
    uint8_t* tag_ptr = ptr + 8;

    uint32_t max_mod_end = 0;
    int capped_logged = 0;
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
                    if (addr + len > DIRECT_MAP_LIMIT) {
                        if (!capped_logged) {
                            kprintf("[pmm] RAM exceeds 768MB direct map limit; using first 768MB\n");
                            capped_logged = 1;
                        }
                        if (addr >= DIRECT_MAP_LIMIT) {
                            continue;
                        }
                        len = DIRECT_MAP_LIMIT - addr;
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

    /* Now find a safe early memory location for frame_refcount above kernel_end,
       multiboot info, and any modules */
    uint32_t safe_start = (uint32_t)&kernel_end;
    if (mb_info_addr) {
        uint32_t mb_end_virt = mb_info_addr + total_size;
        if (mb_end_virt > safe_start) safe_start = mb_end_virt;
    }
    if (max_mod_end > 0) {
        uint32_t mod_end_virt = (uint32_t)P2V(max_mod_end);
        if (mod_end_virt > safe_start) safe_start = mod_end_virt;
    }

    uint32_t refcount_start = (safe_start + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
    uint32_t refcount_bytes = MAX_FRAMES * sizeof(uint16_t);
    uint32_t reserved_early_end_phys = V2P(refcount_start + refcount_bytes);

    frame_refcount = (uint16_t*) refcount_start;

    /* Initialize refcounts based on bitmap */
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        frame_refcount[i] = bitmap_test(i) ? 1 : 0;
    }

    /* 1. Reserve low 1MB (BIOS/real-mode/VGA memory) */
    mark_region_used_bytes(0, 0x100000);

    /* 2. Reserve kernel image + early frame_refcount array */
    if (reserved_early_end_phys > 0x100000) {
        mark_region_used_bytes(0x100000, reserved_early_end_phys - 0x100000);
    }

    /* 3. Reserve Multiboot2 info structure */
    if (mb_info_addr && total_size > 0) {
        uint32_t mb_info_phys = V2P(mb_info_addr);
        mark_region_used_bytes(mb_info_phys, total_size);
    }

    /* 4. Reserve Multiboot2 modules (if present) */
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
            (total_usable_frames * 4) / 1024, total_usable_frames,
            (free_frames * 4) / 1024, free_frames);
    terminal_writestring("[ok] Physical memory manager initialized\n");
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t frame = 0; frame <= highest_frame; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            frame_refcount[frame] = 1;
            free_frames--;
            return frame * FRAME_SIZE;
        }
    }
    return 0; /* out of memory */
}

void pmm_ref(uint32_t phys_addr) {
    uint32_t frame = phys_addr / FRAME_SIZE;
    if (frame < MAX_FRAMES) {
        frame_refcount[frame]++;
    }
}

void pmm_unref(uint32_t phys_addr) {
    uint32_t frame = phys_addr / FRAME_SIZE;
    if (frame >= MAX_FRAMES) return;

    if (frame_refcount[frame] == 0) {
        kprintf("[pmm] ASSERTION FAILED: unref on frame 0x%08x with refcount 0\n", phys_addr);
        return;
    }

    frame_refcount[frame]--;
    if (frame_refcount[frame] == 0) {
        bitmap_clear(frame);
        free_frames++;
    }
}

void pmm_free_frame(uint32_t phys_addr) {
    pmm_unref(phys_addr);
}

uint16_t pmm_refcount(uint32_t phys_addr) {
    uint32_t frame = phys_addr / FRAME_SIZE;
    return (frame < MAX_FRAMES) ? frame_refcount[frame] : 0;
}

uint32_t pmm_free_count(void) {
    return free_frames;
}

uint32_t pmm_free_frame_count(void) {
    return free_frames;
}

uint32_t pmm_total_count(void) {
    return total_usable_frames;
}

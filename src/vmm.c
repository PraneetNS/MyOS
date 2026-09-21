#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4

static void track_frame(address_space_t* as, uint32_t frame_phys) {
    if (as->owned_count < VMM_MAX_OWNED_FRAMES)
        as->owned_frames[as->owned_count++] = frame_phys;
    else
        terminal_writestring("[vmm] warning: owned-frame tracking table full, frame not tracked (will leak on exit)\n");
}

address_space_t vmm_create_address_space(void) {
    address_space_t as = {0, 0, {0}, 0};

    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys) {
        terminal_writestring("[vmm] out of physical memory for page directory\n");
        return as;
    }

    /* dir_phys is guaranteed < 16MB by the pmm's allocation order (frames
       are handed out lowest-first, and the kernel's own reserved region
       only extends a couple MB), so it's directly writable right now
       through the KERNEL's own still-active identity-mapped directory. */
    uint32_t* dir = (uint32_t*) dir_phys;
    for (int i = 0; i < 1024; i++) dir[i] = 0;

    dir[0] = paging_get_kernel_dir_entry0(); /* share kernel space, already supervisor-only */

    as.directory = dir;
    as.directory_phys = dir_phys;
    track_frame(&as, dir_phys);
    return as;
}

uint32_t vmm_map_user_page(address_space_t* as, uint32_t vaddr) {
    uint32_t dir_index   = vaddr >> 22;
    uint32_t table_index = (vaddr >> 12) & 0x3FF;

    if (dir_index == 0) {
        terminal_writestring("[vmm] refused: cannot map into kernel space (dir 0)\n");
        return 0;
    }

    uint32_t* dir = as->directory;
    uint32_t* table;

    if (!(dir[dir_index] & PAGE_PRESENT)) {
        uint32_t table_phys = pmm_alloc_frame();
        if (!table_phys) { terminal_writestring("[vmm] out of physical memory for page table\n"); return 0; }
        table = (uint32_t*) table_phys;
        for (int i = 0; i < 1024; i++) table[i] = 0;
        dir[dir_index] = table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        track_frame(as, table_phys);
    } else {
        table = (uint32_t*)(dir[dir_index] & ~0xFFFu);
    }

    uint32_t frame_phys = pmm_alloc_frame();
    if (!frame_phys) { terminal_writestring("[vmm] out of physical memory for a page frame\n"); return 0; }

    table[table_index] = frame_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    track_frame(as, frame_phys);
    return frame_phys;
}

void vmm_destroy_address_space(address_space_t* as) {
    for (int i = 0; i < as->owned_count; i++)
        pmm_free_frame(as->owned_frames[i]);
    as->owned_count = 0;
    as->directory = 0;
    as->directory_phys = 0;
}

void vmm_switch(address_space_t* as) {
    asm volatile ("mov %0, %%cr3" : : "r"(as->directory_phys) : "memory");
}

void vmm_switch_to_kernel(void) {
    asm volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_dir_phys()) : "memory");
}

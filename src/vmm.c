#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"

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

    uint32_t* dir = (uint32_t*) P2V(dir_phys);
    for (int i = 0; i < 1024; i++) dir[i] = 0;

    /* Copy kernel PDEs (entries 768 to 1023) from master boot directory */
    for (int i = 768; i < 1024; i++) {
        dir[i] = boot_page_directory[i];
    }

    as.directory = dir;
    as.directory_phys = dir_phys;
    track_frame(&as, dir_phys);
    return as;
}

uint32_t vmm_map_user_page(address_space_t* as, uint32_t vaddr) {
    uint32_t dir_index   = vaddr >> 22;
    uint32_t table_index = (vaddr >> 12) & 0x3FF;

    if (dir_index >= 768) {
        terminal_writestring("[vmm] refused: cannot map into kernel space (>= 0xC0000000)\n");
        return 0;
    }

    uint32_t* dir = as->directory;
    uint32_t* table;

    if (!(dir[dir_index] & PAGE_PRESENT)) {
        uint32_t table_phys = pmm_alloc_frame();
        if (!table_phys) { terminal_writestring("[vmm] out of physical memory for page table\n"); return 0; }
        table = (uint32_t*) P2V(table_phys);
        for (int i = 0; i < 1024; i++) table[i] = 0;
        dir[dir_index] = table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        track_frame(as, table_phys);
    } else {
        uint32_t table_phys = dir[dir_index] & ~0xFFFu;
        table = (uint32_t*) P2V(table_phys);
    }

    uint32_t frame_phys = pmm_alloc_frame();
    if (!frame_phys) { terminal_writestring("[vmm] out of physical memory for a page frame\n"); return 0; }

    table[table_index] = frame_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    track_frame(as, frame_phys);
    return frame_phys;
}

void vmm_destroy_address_space(address_space_t* as) {
    for (int i = 0; i < as->owned_count; i++)
        pmm_unref(as->owned_frames[i]);
    as->owned_count = 0;
    as->directory = 0;
    as->directory_phys = 0;
}

int vmm_clone_user_pages(address_space_t* dst, address_space_t* src) {
    for (uint32_t dir_index = 0; dir_index < 768; dir_index++) {
        uint32_t dir_entry = src->directory[dir_index];
        if (!(dir_entry & PAGE_PRESENT)) continue;

        uint32_t table_phys = dir_entry & ~0xFFFu;
        uint32_t* table = (uint32_t*) P2V(table_phys);

        for (uint32_t table_index = 0; table_index < 1024; table_index++) {
            uint32_t page_entry = table[table_index];
            if (!(page_entry & PAGE_PRESENT)) continue;

            uint32_t vaddr = (dir_index << 22) | (table_index << 12);

            uint32_t new_frame_phys = vmm_map_user_page(dst, vaddr);
            if (!new_frame_phys) return -1;

            uint32_t src_frame_phys = page_entry & ~0xFFFu;
            const uint8_t* source_page = (const uint8_t*) P2V(src_frame_phys);
            uint8_t* dest_page = (uint8_t*) P2V(new_frame_phys);
            for (int i = 0; i < 4096; i++) dest_page[i] = source_page[i];
        }
    }
    return 0;
}

void vmm_switch(address_space_t* as) {
    asm volatile ("mov %0, %%cr3" : : "r"(as->directory_phys) : "memory");
}

void vmm_switch_to_kernel(void) {
    asm volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_dir_phys()) : "memory");
}

#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"

address_space_t vmm_create_address_space(void) {
    address_space_t as = {0, 0};

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
    return as;
}

int vmm_map_page(address_space_t* as, uint32_t vaddr, uint32_t frame_phys, uint32_t pte_flags) {
    uint32_t dir_index   = vaddr >> 22;
    uint32_t table_index = (vaddr >> 12) & 0x3FF;

    if (dir_index >= 768 || !as || !as->directory) return -1;

    uint32_t* dir = as->directory;
    uint32_t* table;

    if (!(dir[dir_index] & PAGE_PRESENT)) {
        uint32_t table_phys = pmm_alloc_frame();
        if (!table_phys) return -1;
        table = (uint32_t*) P2V(table_phys);
        for (int i = 0; i < 1024; i++) table[i] = 0;
        dir[dir_index] = table_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        uint32_t table_phys = dir[dir_index] & ~0xFFFu;
        table = (uint32_t*) P2V(table_phys);
    }

    table[table_index] = (frame_phys & ~0xFFFu) | pte_flags;
    return 0;
}

uint32_t vmm_map_user_page(address_space_t* as, uint32_t vaddr) {
    uint32_t frame_phys = pmm_alloc_frame();
    if (!frame_phys) return 0;
    if (vmm_map_page(as, vaddr, frame_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_unref(frame_phys);
        return 0;
    }
    return frame_phys;
}

void vmm_unmap_user_page(address_space_t* as, uint32_t vaddr) {
    uint32_t dir_index   = vaddr >> 22;
    uint32_t table_index = (vaddr >> 12) & 0x3FF;

    if (dir_index >= 768 || !as || !as->directory) return;

    uint32_t pde = as->directory[dir_index];
    if (!(pde & PAGE_PRESENT)) return;

    uint32_t table_phys = pde & ~0xFFFu;
    uint32_t* table = (uint32_t*) P2V(table_phys);

    if (table[table_index] & PAGE_PRESENT) {
        uint32_t frame_phys = table[table_index] & ~0xFFFu;
        table[table_index] = 0;
        pmm_unref(frame_phys);
        asm volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");

        /* Check if page table is now empty */
        int empty = 1;
        for (int i = 0; i < 1024; i++) {
            if (table[i] & PAGE_PRESENT) {
                empty = 0;
                break;
            }
        }
        if (empty) {
            as->directory[dir_index] = 0;
            pmm_unref(table_phys);
        }
    }
}

void vmm_destroy_address_space(address_space_t* as) {
    if (!as || !as->directory) return;

    for (uint32_t dir_index = 0; dir_index < 768; dir_index++) {
        uint32_t dir_entry = as->directory[dir_index];
        if (!(dir_entry & PAGE_PRESENT)) continue;

        uint32_t table_phys = dir_entry & ~0xFFFu;
        uint32_t* table = (uint32_t*) P2V(table_phys);

        for (uint32_t table_index = 0; table_index < 1024; table_index++) {
            uint32_t page_entry = table[table_index];
            if (page_entry & PAGE_PRESENT) {
                uint32_t frame_phys = page_entry & ~0xFFFu;
                pmm_unref(frame_phys);
            }
        }
        as->directory[dir_index] = 0;
        pmm_unref(table_phys);
    }

    pmm_unref(as->directory_phys);
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

            uint32_t frame_phys = page_entry & ~0xFFFu;
            uint32_t flags = page_entry & 0xFFFu;

            /* If the page was writable, clear write bit and set COW bit in both */
            if (flags & PAGE_WRITE) {
                flags &= ~PAGE_WRITE;
                flags |= PTE_COW;
                table[table_index] = frame_phys | flags;
            }

            uint32_t vaddr = (dir_index << 22) | (table_index << 12);

            /* Map into destination with identical flags (read-only + COW) */
            if (vmm_map_page(dst, vaddr, frame_phys, flags) != 0) {
                return -1;
            }

            /* Increment reference count for shared frame */
            pmm_ref(frame_phys);
        }
    }

    /* Flush TLB in source address space since writable pages were made read-only */
    paging_flush(src->directory_phys);
    return 0;
}

void vmm_switch(address_space_t* as) {
    asm volatile ("mov %0, %%cr3" : : "r"(as->directory_phys) : "memory");
}

void vmm_switch_to_kernel(void) {
    asm volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_dir_phys()) : "memory");
}

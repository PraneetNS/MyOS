#include "vmm.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"
#include "vma.h"

/* 4-Level Paging Helpers on x86-64:
   Virtual Address 48-bit canonical:
     PML4 index: (vaddr >> 39) & 0x1FF  (512 entries)
     PDPT index: (vaddr >> 30) & 0x1FF  (512 entries)
     PD index:   (vaddr >> 21) & 0x1FF  (512 entries)
     PT index:   (vaddr >> 12) & 0x1FF  (512 entries)
*/

address_space_t vmm_create_address_space(void) {
    address_space_t as = {0, 0};

    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) {
        terminal_writestring("[vmm] out of physical memory for PML4 table\n");
        return as;
    }

    uint64_t* pml4 = (uint64_t*) P2V(pml4_phys);
    for (int i = 0; i < 512; i++) pml4[i] = 0;

    /* Copy kernel higher-half mappings (entries 256..511) from master boot_pml4:
       PML4[256]: Direct Physical Map (0xFFFF800000000000)
       PML4[511]: Kernel Code/Data image (-2GB: 0xFFFFFFFF80000000)
    */
    uint64_t* master_pml4 = paging_get_kernel_pml4_virt();
    for (int i = 256; i < 512; i++) {
        pml4[i] = master_pml4[i];
    }

    as.pml4 = pml4;
    as.pml4_phys = pml4_phys;
    return as;
}

int vmm_map_page(address_space_t* as, uint64_t vaddr, uint64_t frame_phys, uint64_t pte_flags) {
    if (!as || !as->pml4) return -1;

    uint64_t pml4_idx = (vaddr >> 39) & 0x1FFULL;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FFULL;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FFULL;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FFULL;

    /* Disallow user space mappings above canonical lower half */
    if (pml4_idx >= 256) return -1;

    uint64_t* pml4 = as->pml4;

    /* 1. PML4 -> PDPT */
    uint64_t* pdpt;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        uint64_t pdpt_phys = pmm_alloc_frame();
        if (!pdpt_phys) return -1;
        pdpt = (uint64_t*) P2V(pdpt_phys);
        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        pml4[pml4_idx] = pdpt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pdpt = (uint64_t*) P2V(pml4[pml4_idx] & ~0xFFFULL);
    }

    /* 2. PDPT -> PD */
    uint64_t* pd;
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        uint64_t pd_phys = pmm_alloc_frame();
        if (!pd_phys) return -1;
        pd = (uint64_t*) P2V(pd_phys);
        for (int i = 0; i < 512; i++) pd[i] = 0;
        pdpt[pdpt_idx] = pd_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);
    }

    /* 3. PD -> PT */
    uint64_t* pt;
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint64_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return -1;
        pt = (uint64_t*) P2V(pt_phys);
        for (int i = 0; i < 512; i++) pt[i] = 0;
        pd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);
    }

    /* 4. PT entry */
    pt[pt_idx] = (frame_phys & ~0xFFFULL) | pte_flags;
    return 0;
}

uint64_t vmm_map_user_page(address_space_t* as, uint64_t vaddr) {
    uint64_t frame_phys = pmm_alloc_frame();
    if (!frame_phys) return 0;
    if (vmm_map_page(as, vaddr, frame_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX) != 0) {
        pmm_unref(frame_phys);
        return 0;
    }
    return frame_phys;
}

void vmm_unmap_user_page(address_space_t* as, uint64_t vaddr) {
    if (!as || !as->pml4) return;

    uint64_t pml4_idx = (vaddr >> 39) & 0x1FFULL;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FFULL;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FFULL;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FFULL;

    if (pml4_idx >= 256) return;

    if (!(as->pml4[pml4_idx] & PAGE_PRESENT)) return;
    uint64_t* pdpt = (uint64_t*) P2V(as->pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) return;
    uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PAGE_PRESENT)) return;
    uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);

    if (pt[pt_idx] & PAGE_PRESENT) {
        uint64_t frame_phys = pt[pt_idx] & ~0xFFFULL & ~(1ULL << 63);
        pt[pt_idx] = 0;
        if (pmm_is_ram(frame_phys)) {
            pmm_unref(frame_phys);
        }
        invlpg_page(vaddr);
    }
}

void vmm_destroy_address_space(address_space_t* as) {
    if (!as || !as->pml4) return;

    for (uint64_t pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(as->pml4[pml4_idx] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = as->pml4[pml4_idx] & ~0xFFFULL;
        uint64_t* pdpt = (uint64_t*) P2V(pdpt_phys);

        for (uint64_t pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
            uint64_t* pd = (uint64_t*) P2V(pd_phys);

            for (uint64_t pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PAGE_PRESENT)) continue;
                uint64_t pt_phys = pd[pd_idx] & ~0xFFFULL;
                uint64_t* pt = (uint64_t*) P2V(pt_phys);

                for (uint64_t pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (pt[pt_idx] & PAGE_PRESENT) {
                        uint64_t frame_phys = pt[pt_idx] & ~0xFFFULL & ~(1ULL << 63);
                        if (pmm_is_ram(frame_phys)) {
                            pmm_unref(frame_phys);
                        }
                    }
                }
                pmm_unref(pt_phys);
            }
            pmm_unref(pd_phys);
        }
        pmm_unref(pdpt_phys);
    }

    pmm_unref(as->pml4_phys);
    as->pml4 = 0;
    as->pml4_phys = 0;
}

int vmm_clone_user_pages(address_space_t* dst, address_space_t* src, vma_t* vma_list) {
    for (uint64_t pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(src->pml4[pml4_idx] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*) P2V(src->pml4[pml4_idx] & ~0xFFFULL);

        for (uint64_t pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
            uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);

            for (uint64_t pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PAGE_PRESENT)) continue;
                uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);

                for (uint64_t pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t pte = pt[pt_idx];
                    if (!(pte & PAGE_PRESENT)) continue;

                    uint64_t frame_phys = pte & ~0xFFFULL & ~(1ULL << 63);
                    uint64_t flags = pte & 0xFFFULL;
                    uint64_t nx = pte & (1ULL << 63);

                    uint64_t vaddr = (pml4_idx << 39) | (pdpt_idx << 30) | (pd_idx << 21) | (pt_idx << 12);

                    if (pmm_is_ram(frame_phys)) {
                        vma_t* v = vma_list;
                        while (v) {
                            if (v->start <= vaddr && vaddr < v->end) break;
                            v = v->next;
                        }
                        int is_shared = (v && (v->flags & VMA_FLAG_SHARED));

                        if (is_shared) {
                            if (vmm_map_page(dst, vaddr, frame_phys, flags | nx) != 0) return -1;
                            pmm_ref(frame_phys);
                        } else {
                            if (flags & PAGE_WRITE) {
                                flags &= ~PAGE_WRITE;
                                flags |= PTE_COW;
                                pt[pt_idx] = frame_phys | flags | nx;
                            }

                            if (vmm_map_page(dst, vaddr, frame_phys, flags | nx) != 0) return -1;
                            pmm_ref(frame_phys);
                        }
                    } else {
                        if (vmm_map_page(dst, vaddr, frame_phys, flags | nx) != 0) return -1;
                    }
                }
            }
        }
    }

    paging_flush(src->pml4_phys);
    return 0;
}

void vmm_switch(address_space_t* as) {
    asm volatile ("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

void vmm_switch_to_kernel(void) {
    asm volatile ("mov %0, %%cr3" : : "r"(paging_get_kernel_pml4_phys()) : "memory");
}

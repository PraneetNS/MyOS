#ifndef VMM_H
#define VMM_H

#include <stdint.h>

typedef struct {
    uint64_t* pml4;           /* kernel-virtual pointer to PML4 table (512 entries) */
    uint64_t  pml4_phys;      /* physical address to load into CR3 */
} address_space_t;

/* Allocates a fresh 4-level address space.
   Copies the higher-half kernel entries (256..511) from the master kernel PML4;
   user space entries (0..255) start unmapped. */
address_space_t vmm_create_address_space(void);

/* Maps virtual page `vaddr` to physical frame `frame_phys` with given PTE flags */
int vmm_map_page(address_space_t* as, uint64_t vaddr, uint64_t frame_phys, uint64_t pte_flags);

/* Maps one 4KB page at `vaddr` (< 0x0000800000000000) to a fresh physical frame, user-accessible. */
uint64_t vmm_map_user_page(address_space_t* as, uint64_t vaddr);

/* Unmaps virtual page `vaddr` from address space and unrefs the underlying frame */
void vmm_unmap_user_page(address_space_t* as, uint64_t vaddr);

/* Frees all user-space page tables and frames back to the PMM */
void vmm_destroy_address_space(address_space_t* as);

struct vma;
/* Duplicates every user page currently mapped in `src` into `dst` (COW / shared handling) */
int vmm_clone_user_pages(address_space_t* dst, address_space_t* src, struct vma* vma_list);

void vmm_switch(address_space_t* as);
void vmm_switch_to_kernel(void);

#endif

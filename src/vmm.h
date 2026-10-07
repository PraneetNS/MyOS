#ifndef VMM_H
#define VMM_H

#include <stdint.h>

typedef struct {
    uint32_t* directory;      /* kernel-virtual pointer to the page directory */
    uint32_t  directory_phys;
} address_space_t;

/* Allocates a fresh page directory. Copies the kernel PDEs (768..1023)
   from the master kernel page directory; user space (0..767) starts unmapped. */
address_space_t vmm_create_address_space(void);

/* Maps virtual page `vaddr` to physical frame `frame_phys` with given PTE flags */
int vmm_map_page(address_space_t* as, uint32_t vaddr, uint32_t frame_phys, uint32_t pte_flags);

/* Maps one 4KB page at `vaddr` (must be < 0xC0000000) to a FRESH physical
   frame in the given address space, user-accessible. Returns the frame's
   physical address, or 0 on failure. */
uint32_t vmm_map_user_page(address_space_t* as, uint32_t vaddr);

/* Unmaps virtual page `vaddr` from address space and unrefs frame */
void vmm_unmap_user_page(address_space_t* as, uint32_t vaddr);

/* Frees every frame this address space owns back to the pmm. */
void vmm_destroy_address_space(address_space_t* as);

/* Duplicates every user page currently mapped in `src` into `dst`,
   each backed by a FRESH physical frame with the same content. */
int vmm_clone_user_pages(address_space_t* dst, address_space_t* src);

void vmm_switch(address_space_t* as); /* loads CR3 with as->directory_phys */
void vmm_switch_to_kernel(void);      /* restores the kernel's own page directory */

#endif

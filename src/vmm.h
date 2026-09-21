#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define VMM_MAX_OWNED_FRAMES 64

typedef struct {
    uint32_t* directory;      /* kernel-virtual==physical pointer to the page directory */
    uint32_t  directory_phys;
    /* Every physical frame this address space owns (its directory, any
       page tables it allocated, and every data/stack page) -- tracked
       so vmm_destroy_address_space() can actually give them back to the
       pmm instead of leaking them, unlike Stage 5/6's one-shot design. */
    uint32_t  owned_frames[VMM_MAX_OWNED_FRAMES];
    int       owned_count;
} address_space_t;

/* Allocates a fresh page directory. Directory entry 0 (0-4MB, kernel
   space) is shared with the kernel's own directory and supervisor-only;
   everything else starts unmapped. */
address_space_t vmm_create_address_space(void);

/* Maps one 4KB page at `vaddr` (must be >= 4MB -- dir 0 is kernel-only
   and refused) to a FRESH physical frame in the given address space,
   user-accessible. Returns the frame's physical address (so the caller
   can write to it directly while the kernel's own identity-mapped
   directory is still active), or 0 on failure. */
uint32_t vmm_map_user_page(address_space_t* as, uint32_t vaddr);

/* Frees every frame this address space owns (data pages, page tables,
   and the directory itself) back to the pmm. */
void vmm_destroy_address_space(address_space_t* as);

void vmm_switch(address_space_t* as); /* loads CR3 with as->directory_phys */
void vmm_switch_to_kernel(void);      /* restores the kernel's own page directory */

#endif

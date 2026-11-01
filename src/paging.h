#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Virtual Address Space Layout (4-Level 48-bit Canonical Addressing):
   - Direct PhysMap: 0xFFFF800000000000 (covers all physical RAM up to 1TB using 1GB/2MB pages)
   - Kernel Image Base (-2GB): 0xFFFFFFFF80000000
   - User Space: 0x0000000000000000 .. 0x00007FFFFFFFFFFF (lower 128TB)
*/

#define PHYSMAP_BASE         0xFFFF800000000000ULL
#define KERNEL_VIRTUAL_BASE  0xFFFFFFFF80000000ULL

/* P2V and V2P macros for physical RAM in the direct physical map */
#define P2V(phys) ((void*)((uint64_t)(phys) + PHYSMAP_BASE))
#define V2P(virt) ((uint64_t)((uint64_t)(virt) - PHYSMAP_BASE))

/* Kernel image P2V/V2P for static kernel code/data symbols */
#define KERN_P2V(phys) ((void*)((uint64_t)(phys) + KERNEL_VIRTUAL_BASE))
#define KERN_V2P(virt) ((uint64_t)((uint64_t)(virt) - KERNEL_VIRTUAL_BASE))

#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE   (1ULL << 1)
#define PAGE_USER    (1ULL << 2)
#define PAGE_PWT     (1ULL << 3)
#define PAGE_PCD     (1ULL << 4)
#define PAGE_ACCESSED (1ULL << 5)
#define PAGE_DIRTY   (1ULL << 6)
#define PAGE_PSE     (1ULL << 7)
#define PAGE_GLOBAL  (1ULL << 8)
#define PTE_COW      (1ULL << 9)   /* Software Copy-On-Write bit */
#define PAGE_NX      (1ULL << 63)  /* No-Execute bit */

extern uint64_t boot_pml4[512];

void paging_init(void);
uint64_t paging_get_kernel_pml4_phys(void);
uint64_t* paging_get_kernel_pml4_virt(void);
void paging_flush(uint64_t pml4_phys);
void invlpg_page(uint64_t vaddr);

#endif

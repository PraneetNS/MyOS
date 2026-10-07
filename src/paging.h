#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define KERNEL_BASE          0xC0000000u
#define DIRECT_MAP_LIMIT     (768 * 1024 * 1024u)

#define P2V(phys) ((void*)((uintptr_t)(phys) + KERNEL_BASE))
#define V2P(virt) ((uint32_t)((uintptr_t)(virt) - KERNEL_BASE))

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4
#define PAGE_PSE     0x80
#define PTE_COW      0x200u   /* bit 9: software Copy-on-Write bit */

extern uint32_t boot_page_directory[1024];

void paging_init(void);
uint32_t paging_get_kernel_dir_phys(void);

#endif

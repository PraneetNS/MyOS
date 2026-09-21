#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

void paging_init(void);
uint32_t paging_get_kernel_dir_entry0(void); /* raw dword: shared kernel-space mapping for new address spaces */
uint32_t paging_get_kernel_dir_phys(void);   /* physical address of the kernel's own page directory */

#endif

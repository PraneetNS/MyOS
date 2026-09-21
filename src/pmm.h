#ifndef PMM_H
#define PMM_H

#include <stdint.h>

void pmm_init(uint32_t mb_info_addr);
uint32_t pmm_alloc_frame(void);   /* returns physical address of a free 4KB frame, or 0 */
void pmm_free_frame(uint32_t phys_addr);
uint32_t pmm_free_frame_count(void);

#endif

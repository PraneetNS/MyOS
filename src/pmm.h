#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define FRAME_SIZE 4096

void pmm_init(uint32_t mb_info_addr);
uint32_t pmm_alloc_frame(void);   /* returns physical address of a free 4KB frame with refcount=1, or 0 */
void pmm_free_frame(uint32_t phys_addr);
void pmm_ref(uint32_t phys_addr);
void pmm_unref(uint32_t phys_addr);
uint16_t pmm_refcount(uint32_t phys_addr);
uint32_t pmm_free_count(void);
uint32_t pmm_free_frame_count(void); /* backwards compatibility */
uint32_t pmm_total_count(void);

#endif

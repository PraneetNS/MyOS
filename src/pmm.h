#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_SIZE 4096

void     pmm_init(uint64_t mb_info_addr);
uint64_t pmm_alloc_frame(void);   /* returns physical address of a free 4KB frame with refcount=1, or 0 */
uint64_t pmm_alloc_contiguous_frames(size_t count);
uint64_t pmm_alloc_frame_low(void); /* allocates a frame guaranteed below 4GB for legacy virtio DMA */
void     pmm_free_frame(uint64_t phys_addr);
void     pmm_free_contiguous_frames(uint64_t phys_addr, size_t count);
void     pmm_ref(uint64_t phys_addr);
void     pmm_unref(uint64_t phys_addr);
uint16_t pmm_refcount(uint64_t phys_addr);
uint64_t pmm_free_count(void);
uint64_t pmm_free_frame_count(void); /* backwards compatibility */
uint64_t pmm_total_count(void);
int      pmm_is_ram(uint64_t phys_addr);

#endif

#ifndef IOREMAP_H
#define IOREMAP_H

#include <stdint.h>
#include <stddef.h>

#define IOREMAP_START 0xFFFFFFFFA0000000ULL
#define IOREMAP_END   0xFFFFFFFFB0000000ULL

#define IOMAP_NOCACHE        0x01
#define IOMAP_WRITECOMBINING 0x02

void* ioremap(uint64_t phys_addr, size_t size, uint32_t flags);
void  iounmap(void* virt_addr, size_t size);
int   cpu_has_pat(void);
void  pat_init(void);

#endif

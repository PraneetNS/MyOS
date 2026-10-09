#ifndef IOREMAP_H
#define IOREMAP_H

#include <stdint.h>
#include <stddef.h>

#define IOREMAP_START 0xF0000000u
#define IOREMAP_END   0xFFC00000u

#define IOMAP_NOCACHE        0x01
#define IOMAP_WRITECOMBINING 0x02

void* ioremap(uint32_t phys_addr, size_t size, uint32_t flags);
void  iounmap(void* virt_addr, size_t size);
int   cpu_has_pat(void);
void  pat_init(void);

#endif

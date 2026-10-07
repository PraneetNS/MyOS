#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>

#ifndef KHEAP_DEBUG
#define KHEAP_DEBUG 1
#endif

void kheap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);

#if KHEAP_DEBUG
void kheap_selftest(void);
#endif

#endif

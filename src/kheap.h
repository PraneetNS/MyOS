#ifndef KHEAP_H
#define KHEAP_H

#include <stddef.h>

#ifndef KHEAP_DEBUG
#define KHEAP_DEBUG 1
#endif

void kheap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);

void* memset(void* dest, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);

#if KHEAP_DEBUG
void kheap_selftest(void);
#endif

#endif

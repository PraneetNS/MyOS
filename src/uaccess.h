#ifndef _UACCESS_H
#define _UACCESS_H

#include <stdint.h>

extern int smap_enabled;

static inline void user_access_begin(void) {
    if (smap_enabled) __asm__ volatile ("stac" ::: "cc");
}

static inline void user_access_end(void) {
    if (smap_enabled) __asm__ volatile ("clac" ::: "cc");
}

int copy_to_user(void* user_dest, const void* kernel_src, uint32_t len);
int copy_from_user(void* kernel_dest, const void* user_src, uint32_t len);
int validate_user_buffer(const void* ptr, uint64_t len, int write);
int validate_user_string(const char* s);

extern char copy_user_start[];
extern char copy_user_end[];
extern char copy_user_start2[];
extern char copy_user_end2[];
extern void copy_user_fault(void);

#endif

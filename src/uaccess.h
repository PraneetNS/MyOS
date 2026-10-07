#ifndef _UACCESS_H
#define _UACCESS_H

#include <stdint.h>

int copy_to_user(void* user_dest, const void* kernel_src, uint32_t len);
int copy_from_user(void* kernel_dest, const void* user_src, uint32_t len);

extern char copy_user_start[];
extern char copy_user_end[];
extern char copy_user_start2[];
extern char copy_user_end2[];
extern void copy_user_fault(void);

#endif

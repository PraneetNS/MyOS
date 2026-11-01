#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>
#include <stddef.h>

void random_init(void);
void random_add_entropy(const void* data, size_t len);
void random_get_bytes(void* buf, size_t len);
uint32_t random_u32(void);
void random_save_seed(void);

#endif


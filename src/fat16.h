#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include "vfs.h"

int fat16_init(void);
vnode_t* fat16_get_root(void);

#endif

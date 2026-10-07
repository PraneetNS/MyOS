#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

void procfs_init(void);
vnode_t* procfs_get_root(void);

#endif

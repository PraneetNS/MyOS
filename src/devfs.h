#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

void devfs_init(void);
vnode_t* devfs_get_root(void);
int devfs_is_tty_vnode(vnode_t* vn);

#endif

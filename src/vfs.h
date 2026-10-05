#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "errno.h"

typedef enum {
    VNODE_FILE = 1,
    VNODE_DIR,
    VNODE_CONSOLE,
    VNODE_PIPE
} vnode_type_t;

typedef enum {
    OPEN_FILE_NONE = 0,
    OPEN_FILE_VNODE,
    OPEN_FILE_CONSOLE,
    OPEN_FILE_PIPE
} open_file_type_t;

/* Open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

/* Seek constants */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* File modes */
#define S_IFMT      0xF000
#define S_IFREG     0x8000
#define S_IFDIR     0x4000
#define S_IFCHR     0x2000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
};

struct dirent {
    uint32_t d_ino;
    char d_name[64];
    uint32_t d_type; /* 1 = file, 2 = dir */
    uint32_t d_size;
};

struct vnode;

struct fs_ops {
    int (*lookup)(struct vnode* dir, const char* name, struct vnode** out);
    int (*read)(struct vnode* node, uint32_t offset, uint8_t* buf, uint32_t count);
    int (*write)(struct vnode* node, uint32_t offset, const uint8_t* buf, uint32_t count);
    int (*create)(struct vnode* dir, const char* name, struct vnode** out);
    int (*mkdir)(struct vnode* dir, const char* name, struct vnode** out);
    int (*unlink)(struct vnode* dir, const char* name);
    int (*rmdir)(struct vnode* dir, const char* name);
    int (*rename)(struct vnode* old_dir, const char* old_name, struct vnode* new_dir, const char* new_name);
    int (*readdir)(struct vnode* dir, uint32_t index, struct dirent* entry);
    int (*truncate)(struct vnode* node, uint32_t length);
    int (*stat)(struct vnode* node, struct stat* st);
};

typedef struct vnode {
    vnode_type_t type;
    uint32_t size;
    void* fs_data;
    const struct fs_ops* ops;
    int refcount;
    struct vnode* parent;
} vnode_t;

typedef struct open_file {
    open_file_type_t type;
    vnode_t* vnode;
    uint32_t offset;
    int flags;
    int refcount;
} open_file_t;

void vfs_init(void);
int vfs_mount(const char* path, vnode_t* root_vnode);
vnode_t* vfs_get_root(void);

/* Vnode allocator and reference counting */
vnode_t* vnode_alloc(vnode_type_t type, const struct fs_ops* ops, void* fs_data);
void vnode_ref(vnode_t* vn);
void vnode_unref(vnode_t* vn);

/* Open file management */
open_file_t* open_file_alloc(open_file_type_t type, vnode_t* vn, int flags);
void open_file_ref(open_file_t* of);
void open_file_unref(open_file_t* of);

/* Path resolution */
int vfs_resolve_path(const char* path, const char* cwd, vnode_t** out);
int vfs_resolve_parent(const char* path, const char* cwd, vnode_t** dir_out, char* name_out, int name_max);

/* Console open files for fds 0, 1, 2 */
open_file_t* vfs_get_console_stdin(void);
open_file_t* vfs_get_console_stdout(void);
open_file_t* vfs_get_console_stderr(void);

/* Standard VFS operations on open_file */
int vfs_read(open_file_t* of, void* buf, uint32_t count);
int vfs_write(open_file_t* of, const void* buf, uint32_t count);
int vfs_close(open_file_t* of);
int vfs_lseek(open_file_t* of, int offset, int whence);
int vfs_readdir(open_file_t* of, struct dirent* entry);
int vfs_stat(vnode_t* vn, struct stat* st);
void vfs_path_canonical(const char* path, const char* cwd, char* out, int max);

#endif

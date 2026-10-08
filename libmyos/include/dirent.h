#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>
#include <stdint.h>

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     2
#define DT_BLK     6
#define DT_REG     1
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    uint32_t     d_ino;
    char         d_name[64];
    uint32_t     d_type;
    uint32_t     d_size;
};

typedef struct {
    int fd;
    struct dirent ent;
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);
void seekdir(DIR *dirp, long loc);
long telldir(DIR *dirp);
int getdents(int fd, void *dirp, size_t count);

#endif

#ifndef FS_H
#define FS_H

#include <stdint.h>

#define FS_MAX_NAME 48

typedef struct {
    char name[FS_MAX_NAME];
    uint32_t start_lba;
    uint32_t size_bytes;
} fs_entry_t;

void fs_init(void);
int fs_list(void);                       /* prints all files, returns count */
const fs_entry_t* fs_find(const char* name);
/* Reads the whole file into buffer (caller must kmalloc entry->size_bytes,
   rounded up to a sector). Returns bytes read, or -1 on error. */
int fs_read_file(const fs_entry_t* entry, uint8_t* buffer);

/* Reads up to maxlen bytes starting at byte offset `offset` within the
   file (for SYS_READ-style partial/streamed reads). Returns bytes
   actually read (0 at end-of-file), or -1 on error. */
int fs_read_range(const fs_entry_t* entry, uint32_t offset, uint8_t* buffer, uint32_t maxlen);

#endif

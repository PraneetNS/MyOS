#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

#define BCACHE_BLOCK_SIZE 512
#define BCACHE_NUM_BLOCKS 64

void bcache_init(void);

/* Reads a 512-byte block at `lba`. Returns 0 on success, -1 on error. */
int bcache_read(uint32_t lba, uint8_t* out);

/* Writes a 512-byte block at `lba`. Returns 0 on success, -1 on error. */
int bcache_write(uint32_t lba, const uint8_t* in);

/* Flushes all dirty cached blocks to disk and triggers ATA cache flush. Returns 0 on success, -1 on error. */
int bcache_sync(void);

#endif

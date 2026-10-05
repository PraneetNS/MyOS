#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Reads one 512-byte sector at the given LBA (primary ATA bus, master
   drive) into buffer. Returns 0 on success, -1 on error/timeout. */
int ata_read_sector(uint32_t lba, uint8_t* buffer);

/* Writes one 512-byte sector at the given LBA (primary ATA bus, master
   drive) from buffer. Returns 0 on success, -1 on error/timeout. */
int ata_write_sector(uint32_t lba, const uint8_t* buffer);

/* Flushes the ATA drive write cache (command 0xE7). Returns 0 on success, -1 on error. */
int ata_flush(void);

#endif

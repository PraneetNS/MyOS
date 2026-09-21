#include "ata.h"
#include "io.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ_SECTORS 0x20

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

static int ata_wait_ready(void) {
    /* Poll status until BSY clears; bail out if ERR sets. A real driver
       would also handle a timeout for a genuinely absent/faulty drive --
       omitted here since QEMU's virtual disk always responds. */
    for (;;) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
}

int ata_read_sector(uint32_t lba, uint8_t* buffer) {
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); /* master drive, LBA mode, top 4 LBA bits */
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_ready() != 0) return -1;

    /* 256 16-bit words = 512 bytes, transferred via the data port */
    uint16_t* buf16 = (uint16_t*) buffer;
    for (int i = 0; i < 256; i++) {
        uint16_t data;
        asm volatile ("inw %1, %0" : "=a"(data) : "Nd"((uint16_t)ATA_DATA));
        buf16[i] = data;
    }

    return 0;
}

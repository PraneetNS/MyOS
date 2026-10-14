#include "bcache.h"
#include "ata.h"
#include "kmutex.h"

typedef struct {
    uint32_t lba;
    uint8_t data[BCACHE_BLOCK_SIZE];
    uint8_t valid;
    uint8_t dirty;
    uint32_t last_used;
} bcache_entry_t;

static bcache_entry_t cache[BCACHE_NUM_BLOCKS];
static uint32_t access_counter = 0;
static kmutex_t bcache_mutex;

void bcache_init(void) {
    kmutex_init(&bcache_mutex, "bcache");
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        cache[i].valid = 0;
        cache[i].dirty = 0;
        cache[i].lba = 0;
        cache[i].last_used = 0;
    }
    access_counter = 0;
}

static int find_slot(uint32_t lba) {
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        if (cache[i].valid && cache[i].lba == lba) {
            return i;
        }
    }
    return -1;
}

static int pick_victim(void) {
    /* 1. Look for an unused slot first */
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        if (!cache[i].valid) return i;
    }

    /* 2. Simple LRU: lowest last_used timestamp */
    int victim = 0;
    uint32_t oldest = cache[0].last_used;
    for (int i = 1; i < BCACHE_NUM_BLOCKS; i++) {
        if (cache[i].last_used < oldest) {
            oldest = cache[i].last_used;
            victim = i;
        }
    }

    /* If dirty, flush to disk before evicting (ATA transfer with interrupts enabled) */
    if (cache[victim].dirty) {
        if (ata_write_sector(cache[victim].lba, cache[victim].data) != 0) {
            return -1;
        }
        cache[victim].dirty = 0;
    }

    cache[victim].valid = 0;
    return victim;
}

int bcache_read(uint32_t lba, uint8_t* out) {
    kmutex_lock(&bcache_mutex);
    access_counter++;
    int slot = find_slot(lba);
    if (slot >= 0) {
        cache[slot].last_used = access_counter;
        for (int i = 0; i < BCACHE_BLOCK_SIZE; i++) {
            out[i] = cache[slot].data[i];
        }
        kmutex_unlock(&bcache_mutex);
        return 0;
    }

    /* Miss: pick victim and read from ATA (interrupts enabled) */
    slot = pick_victim();
    if (slot < 0) {
        kmutex_unlock(&bcache_mutex);
        return -1;
    }

    if (ata_read_sector(lba, cache[slot].data) != 0) {
        kmutex_unlock(&bcache_mutex);
        return -1;
    }

    cache[slot].lba = lba;
    cache[slot].valid = 1;
    cache[slot].dirty = 0;
    cache[slot].last_used = access_counter;

    for (int i = 0; i < BCACHE_BLOCK_SIZE; i++) {
        out[i] = cache[slot].data[i];
    }
    kmutex_unlock(&bcache_mutex);
    return 0;
}

int bcache_write(uint32_t lba, const uint8_t* in) {
    kmutex_lock(&bcache_mutex);
    access_counter++;
    int slot = find_slot(lba);
    if (slot < 0) {
        slot = pick_victim();
        if (slot < 0) {
            kmutex_unlock(&bcache_mutex);
            return -1;
        }
        cache[slot].lba = lba;
        cache[slot].valid = 1;
    }

    for (int i = 0; i < BCACHE_BLOCK_SIZE; i++) {
        cache[slot].data[i] = in[i];
    }
    cache[slot].dirty = 1;
    cache[slot].last_used = access_counter;
    kmutex_unlock(&bcache_mutex);
    return 0;
}

int bcache_sync(void) {
    kmutex_lock(&bcache_mutex);
    int err = 0;
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        if (cache[i].valid && cache[i].dirty) {
            if (ata_write_sector(cache[i].lba, cache[i].data) != 0) {
                err = -1;
            } else {
                cache[i].dirty = 0;
            }
        }
    }
    if (ata_flush() != 0) {
        err = -1;
    }
    kmutex_unlock(&bcache_mutex);
    return err;
}

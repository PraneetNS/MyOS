#ifndef PBUF_H
#define PBUF_H

#include <stdint.h>
#include <stddef.h>

#define PBUF_PAYLOAD_SIZE 1536
#define PBUF_POOL_SIZE    128

typedef struct pbuf {
    uint8_t payload[PBUF_PAYLOAD_SIZE];
    uint16_t len;
    uint16_t offset;
    int refcount;
    struct pbuf* next;
} pbuf_t;

void pbuf_pool_init(void);
pbuf_t* pbuf_alloc(void);
void pbuf_free(pbuf_t* p);
pbuf_t* pbuf_ref(pbuf_t* p);

uint32_t pbuf_pool_free_count(void);
uint32_t pbuf_pool_dropped_count(void);

#endif


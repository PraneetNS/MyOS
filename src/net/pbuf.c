#include "pbuf.h"
#include <string.h>

static pbuf_t s_pool[PBUF_POOL_SIZE];
static pbuf_t* s_free_list = NULL;
static uint32_t s_free_count = 0;
static uint32_t s_dropped_count = 0;

void pbuf_pool_init(void) {
    memset(s_pool, 0, sizeof(s_pool));
    s_free_list = NULL;
    for (int i = 0; i < PBUF_POOL_SIZE; i++) {
        s_pool[i].next = s_free_list;
        s_free_list = &s_pool[i];
    }
    s_free_count = PBUF_POOL_SIZE;
    s_dropped_count = 0;
}

pbuf_t* pbuf_alloc(void) {
    uint32_t flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags));

    if (!s_free_list) {
        s_dropped_count++;
        asm volatile ("push %0; popf" : : "r"(flags));
        return NULL;
    }

    pbuf_t* p = s_free_list;
    s_free_list = p->next;
    s_free_count--;

    p->len = 0;
    p->offset = 0;
    p->refcount = 1;
    p->next = NULL;

    asm volatile ("push %0; popf" : : "r"(flags));
    return p;
}

pbuf_t* pbuf_ref(pbuf_t* p) {
    if (!p) return NULL;
    uint32_t flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags));
    p->refcount++;
    asm volatile ("push %0; popf" : : "r"(flags));
    return p;
}

void pbuf_free(pbuf_t* p) {
    if (!p) return;
    uint32_t flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags));

    p->refcount--;
    if (p->refcount <= 0) {
        p->next = s_free_list;
        s_free_list = p;
        s_free_count++;
    }

    asm volatile ("push %0; popf" : : "r"(flags));
}

uint32_t pbuf_pool_free_count(void) {
    return s_free_count;
}

uint32_t pbuf_pool_dropped_count(void) {
    return s_dropped_count;
}


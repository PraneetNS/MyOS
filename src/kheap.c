#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define KHEAP_MAGIC_HEAD  0xDEADBEEFu
#define KHEAP_MAGIC_TAIL  0xCAFEBABEu
#define KHEAP_FOOTER_SIZE 16
#define KHEAP_CHUNK_MIN   (256 * 1024)  /* 256KB minimum chunk size */

typedef struct block_header {
    uint32_t magic;             /* 0xDEADBEEF */
    size_t size;                /* usable capacity of block (bytes, multiple of 16) */
    size_t req_size;            /* requested size */
    uint32_t free;              /* 1 if free, 0 if allocated */
    struct block_header* next;  /* next block in heap */
    struct block_header* prev;  /* prev block in heap */
    uint32_t padding[2];        /* align sizeof(block_header_t) to 32 bytes */
} block_header_t;

_Static_assert(sizeof(block_header_t) == 32, "block_header_t must be 32 bytes");

static block_header_t* heap_head = 0;

static void kheap_panic(const char* msg, void* ptr, uint32_t val1, uint32_t val2) {
    kprintf("\n*** KERNEL PANIC [kheap]: %s (ptr=%p, 0x%08x vs 0x%08x) ***\nSystem halted.\n",
            msg, ptr, val1, val2);
    serial_printf("\n*** KERNEL PANIC [kheap]: %s (ptr=%p, 0x%08x vs 0x%08x) ***\nSystem halted.\n",
                  msg, ptr, val1, val2);
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

static void kheap_verify_block(block_header_t* block) {
    if (!block) return;
    if (block->magic != KHEAP_MAGIC_HEAD) {
        kheap_panic("corrupted block header magic", block, block->magic, KHEAP_MAGIC_HEAD);
    }
    if (!block->free) {
        uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
        if (*tail != KHEAP_MAGIC_TAIL) {
            kheap_panic("heap buffer overflow (tail canary corrupted)", block, *tail, KHEAP_MAGIC_TAIL);
        }
    }
}

static block_header_t* kheap_expand(size_t size, block_header_t* last) {
    size_t needed = sizeof(block_header_t) + size + KHEAP_FOOTER_SIZE;
    size_t chunk_size = (needed > KHEAP_CHUNK_MIN) ? needed : KHEAP_CHUNK_MIN;
    chunk_size = (chunk_size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);

    size_t num_frames = chunk_size / FRAME_SIZE;
    uint32_t phys = pmm_alloc_contiguous_frames(num_frames);
    if (!phys && chunk_size > needed) {
        num_frames = (needed + FRAME_SIZE - 1) / FRAME_SIZE;
        phys = pmm_alloc_contiguous_frames(num_frames);
    }
    if (!phys) {
        return 0;
    }

    block_header_t* new_block = (block_header_t*) P2V(phys);
    new_block->magic = KHEAP_MAGIC_HEAD;
    new_block->size = (num_frames * FRAME_SIZE) - sizeof(block_header_t) - KHEAP_FOOTER_SIZE;
    new_block->req_size = 0;
    new_block->free = 1;
    new_block->next = 0;
    new_block->prev = last;

    if (last) {
        last->next = new_block;
        /* If physically contiguous with last and last is free, merge! */
        uint8_t* last_end = (uint8_t*)last + sizeof(block_header_t) + last->size + KHEAP_FOOTER_SIZE;
        if (last_end == (uint8_t*)new_block && last->free) {
            last->size += sizeof(block_header_t) + new_block->size + KHEAP_FOOTER_SIZE;
            last->next = 0;
            return last;
        }
    } else {
        heap_head = new_block;
    }

    return new_block;
}

void kheap_init(void) {
    heap_head = 0;
    /* Allocate initial chunk from direct map */
    kheap_expand(KHEAP_CHUNK_MIN, 0);

    terminal_writestring("[ok] Kernel heap initialized (dynamic, direct-map)\n");

#if KHEAP_DEBUG
    kheap_selftest();
#endif
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;

    size = (size + 15) & ~((size_t)15);

    block_header_t* block = heap_head;
    block_header_t* last = 0;

    while (block) {
        kheap_verify_block(block);
        if (block->free && block->size >= size) {
            /* Split if remainder is large enough */
            size_t min_split = sizeof(block_header_t) + 16 + KHEAP_FOOTER_SIZE;
            if (block->size >= size + min_split) {
                block_header_t* remainder =
                    (block_header_t*)((uint8_t*)block + sizeof(block_header_t) + size + KHEAP_FOOTER_SIZE);
                remainder->magic = KHEAP_MAGIC_HEAD;
                remainder->size = block->size - size - sizeof(block_header_t) - KHEAP_FOOTER_SIZE;
                remainder->req_size = 0;
                remainder->free = 1;
                remainder->next = block->next;
                remainder->prev = block;
                if (remainder->next) {
                    remainder->next->prev = remainder;
                }
                block->next = remainder;
                block->size = size;
            }

            block->free = 0;
            block->req_size = size;
            uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
            *tail = KHEAP_MAGIC_TAIL;
            return (void*)((uint8_t*)block + sizeof(block_header_t));
        }
        last = block;
        block = block->next;
    }

    /* Heap needs to grow */
    block = kheap_expand(size, last);
    if (!block) return 0;

    size_t min_split = sizeof(block_header_t) + 16 + KHEAP_FOOTER_SIZE;
    if (block->size >= size + min_split) {
        block_header_t* remainder =
            (block_header_t*)((uint8_t*)block + sizeof(block_header_t) + size + KHEAP_FOOTER_SIZE);
        remainder->magic = KHEAP_MAGIC_HEAD;
        remainder->size = block->size - size - sizeof(block_header_t) - KHEAP_FOOTER_SIZE;
        remainder->req_size = 0;
        remainder->free = 1;
        remainder->next = block->next;
        remainder->prev = block;
        if (remainder->next) {
            remainder->next->prev = remainder;
        }
        block->next = remainder;
        block->size = size;
    }

    block->free = 0;
    block->req_size = size;
    uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
    *tail = KHEAP_MAGIC_TAIL;
    return (void*)((uint8_t*)block + sizeof(block_header_t));
}

void kfree(void* ptr) {
    if (!ptr) return;

    uintptr_t v = (uintptr_t) ptr;
    if ((v & 15) != 0 || v < KERNEL_BASE || v >= (KERNEL_BASE + DIRECT_MAP_LIMIT)) {
        kheap_panic("invalid pointer passed to kfree", ptr, v, 0);
    }

    block_header_t* block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));

    /* Check magic header */
    if (block->magic != KHEAP_MAGIC_HEAD) {
        kheap_panic("corrupted block header magic (underflow or invalid ptr)", ptr, block->magic, KHEAP_MAGIC_HEAD);
    }

    /* Check double free */
    if (block->free) {
        kheap_panic("double free detected", ptr, block->free, 0);
    }

    /* Check tail canary */
    uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
    if (*tail != KHEAP_MAGIC_TAIL) {
        kheap_panic("buffer overflow detected on free", ptr, *tail, KHEAP_MAGIC_TAIL);
    }

    /* Clear tail canary */
    *tail = 0;
    block->free = 1;
    block->req_size = 0;

    /* Merge with next block if adjacent and free */
    if (block->next && block->next->free) {
        uint8_t* expected_next = (uint8_t*)block + sizeof(block_header_t) + block->size + KHEAP_FOOTER_SIZE;
        if ((uint8_t*)block->next == expected_next) {
            kheap_verify_block(block->next);
            block->size += sizeof(block_header_t) + block->next->size + KHEAP_FOOTER_SIZE;
            block->next = block->next->next;
            if (block->next) {
                block->next->prev = block;
            }
        }
    }

    /* Merge with prev block if adjacent and free */
    if (block->prev && block->prev->free) {
        uint8_t* expected_prev_end = (uint8_t*)block->prev + sizeof(block_header_t) + block->prev->size + KHEAP_FOOTER_SIZE;
        if (expected_prev_end == (uint8_t*)block) {
            kheap_verify_block(block->prev);
            block->prev->size += sizeof(block_header_t) + block->size + KHEAP_FOOTER_SIZE;
            block->prev->next = block->next;
            if (block->next) {
                block->next->prev = block->prev;
            }
        }
    }
}

#if KHEAP_DEBUG
void kheap_selftest(void) {
    kprintf("[kheap] Running kernel heap hardening self-test...\n");
    serial_printf("[kheap] Running kernel heap hardening self-test...\n");

    #define TEST_BLOCKS 64
    void* ptrs[TEST_BLOCKS];
    size_t sizes[TEST_BLOCKS];

    /* 1. Allocate blocks of varying sizes and fill with patterns */
    for (int i = 0; i < TEST_BLOCKS; i++) {
        sizes[i] = ((i % 16) + 1) * 32; /* 32 to 512 bytes */
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            kheap_panic("selftest: kmalloc failed", (void*)(uintptr_t)i, sizes[i], 0);
        }
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            p[j] = (uint8_t)((i * 17 + j) ^ 0x5A);
        }
    }

    /* 2. Verify all patterns */
    for (int i = 0; i < TEST_BLOCKS; i++) {
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)((i * 17 + j) ^ 0x5A)) {
                kheap_panic("selftest: pattern mismatch", ptrs[i], p[j], (i * 17 + j) ^ 0x5A);
            }
        }
    }

    /* 3. Free even blocks */
    for (int i = 0; i < TEST_BLOCKS; i += 2) {
        kfree(ptrs[i]);
        ptrs[i] = 0;
    }

    /* 4. Verify odd blocks still intact */
    for (int i = 1; i < TEST_BLOCKS; i += 2) {
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)((i * 17 + j) ^ 0x5A)) {
                kheap_panic("selftest: odd pattern corrupted", ptrs[i], p[j], (i * 17 + j) ^ 0x5A);
            }
        }
    }

    /* 5. Free odd blocks (triggering coalescing) */
    for (int i = 1; i < TEST_BLOCKS; i += 2) {
        kfree(ptrs[i]);
        ptrs[i] = 0;
    }

    /* 6. Test large allocation requiring direct-map chunk expansion */
    size_t big_size = 128 * 1024; /* 128KB */
    void* big_ptr = kmalloc(big_size);
    if (!big_ptr) {
        kheap_panic("selftest: big kmalloc failed", 0, big_size, 0);
    }
    uint8_t* bp = (uint8_t*)big_ptr;
    for (size_t j = 0; j < big_size; j += 1024) {
        bp[j] = (uint8_t)(j & 0xFF);
    }
    for (size_t j = 0; j < big_size; j += 1024) {
        if (bp[j] != (uint8_t)(j & 0xFF)) {
            kheap_panic("selftest: big pattern corrupted", big_ptr, bp[j], j & 0xFF);
        }
    }
    kfree(big_ptr);

    kprintf("[ok] Kernel heap self-test passed (canaries, coalescing, dynamic growth)\n");
    serial_printf("[ok] Kernel heap self-test passed (canaries, coalescing, dynamic growth)\n");
}
#endif

void* memset(void* dest, int c, size_t n) {
    uint8_t* p = (uint8_t*) dest;
    while (n--) *p++ = (uint8_t) c;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*) dest;
    const uint8_t* s = (const uint8_t*) src;
    while (n--) *d++ = *s++;
    return dest;
}

#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"
#include "serial.h"
#include "io.h"
#include "idt.h"
#include <stdint.h>
#include <stddef.h>

#define KHEAP_MAGIC_HEAD  0xDEADBEEFu
#define KHEAP_MAGIC_TAIL  0xCAFEBABEu
#define KHEAP_FOOTER_SIZE 16
#define KHEAP_CHUNK_MIN   (256 * 1024)  /* 256KB minimum chunk size */

static inline uint64_t rdtsc_cycles(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t g_kheap_max_kmalloc_cycles = 0;
static uint64_t g_kheap_max_kfree_cycles = 0;

uint64_t kheap_get_max_kmalloc_cycles(void) { return g_kheap_max_kmalloc_cycles; }
uint64_t kheap_get_max_kfree_cycles(void) { return g_kheap_max_kfree_cycles; }

typedef struct block_header {
    uint32_t magic;             /* 0xDEADBEEF */
    uint32_t free;              /* 1 if free, 0 if allocated */
    size_t size;                /* usable capacity of block (bytes, multiple of 16) */
    size_t req_size;            /* requested size */
    struct block_header* next;  /* next block in heap */
    struct block_header* prev;  /* prev block in heap */
    uint64_t _pad;              /* pad to 48 bytes (multiple of 16) for 16-byte alignment */
} block_header_t;

static block_header_t* heap_head = 0;

static void kheap_panic(const char* msg, void* ptr, uint64_t val1, uint64_t val2) {
    kprintf("\n*** KERNEL PANIC [kheap]: %s (ptr=%p, 0x%lx vs 0x%lx) ***\nSystem halted.\n",
            msg, ptr, (unsigned long)val1, (unsigned long)val2);
    serial_printf("\n*** KERNEL PANIC [kheap]: %s (ptr=%p, 0x%lx vs 0x%lx) ***\nSystem halted.\n",
                  msg, ptr, (unsigned long)val1, (unsigned long)val2);
    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

static void kheap_verify_block(block_header_t* block) {
    if (!block) return;
    if (block->magic != KHEAP_MAGIC_HEAD) {
        kheap_panic("corrupted block header magic", block, block->magic, KHEAP_MAGIC_HEAD);
    }
    if (block->next && (uintptr_t)block->next < PHYSMAP_BASE) {
        serial_printf("\n[kheap] CORRUPTED NEXT: block=%p next=%p size=%lu req=%lu free=%u\n",
                      block, block->next, (unsigned long)block->size, (unsigned long)block->req_size, block->free);
        uint8_t* p = (uint8_t*)block;
        for (int r = 0; r < 96; r += 16) {
            serial_printf("  +%02x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                          r, p[r], p[r+1], p[r+2], p[r+3], p[r+4], p[r+5], p[r+6], p[r+7],
                          p[r+8], p[r+9], p[r+10], p[r+11], p[r+12], p[r+13], p[r+14], p[r+15]);
        }
        kheap_panic("corrupted block->next pointer", block, (uint64_t)block->next, PHYSMAP_BASE);
    }
    if (block->prev && (uintptr_t)block->prev < PHYSMAP_BASE) {
        serial_printf("\n[kheap] CORRUPTED PREV: block=%p prev=%p size=%lu req=%lu free=%u\n",
                      block, block->prev, (unsigned long)block->size, (unsigned long)block->req_size, block->free);
        kheap_panic("corrupted block->prev pointer", block, (uint64_t)block->prev, PHYSMAP_BASE);
    }
    if (!block->free) {
        uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
        if (*tail != KHEAP_MAGIC_TAIL) {
            serial_printf("\n[kheap] CORRUPTED TAIL: block=%p req_size=%lu size=%lu tail_ptr=%p *tail=0x%x\n",
                          block, (unsigned long)block->req_size, (unsigned long)block->size, tail, *tail);
            uint8_t* p = (uint8_t*)block;
            int total = (int)(sizeof(block_header_t) + block->size + 32);
            if (total > 256) total = 256;
            for (int r = 0; r < total; r += 16) {
                serial_printf("  +%04x (%p): %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                              r, p + r,
                              p[r+0], p[r+1], p[r+2], p[r+3], p[r+4], p[r+5], p[r+6], p[r+7],
                              p[r+8], p[r+9], p[r+10], p[r+11], p[r+12], p[r+13], p[r+14], p[r+15]);
            }
            kheap_panic("heap buffer overflow (tail canary corrupted)", block, *tail, KHEAP_MAGIC_TAIL);
        }
    }
}

static block_header_t* kheap_expand(size_t size, block_header_t* last) {
    size_t needed = sizeof(block_header_t) + size + KHEAP_FOOTER_SIZE;
    size_t chunk_size = (needed > KHEAP_CHUNK_MIN) ? needed : KHEAP_CHUNK_MIN;
    chunk_size = (chunk_size + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);

    size_t num_frames = chunk_size / FRAME_SIZE;
    uint64_t phys = pmm_alloc_contiguous_frames(num_frames);
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
    kheap_expand(2 * KHEAP_CHUNK_MIN, 0);
    terminal_writestring("[ok] Kernel heap initialized (dynamic, direct-map)\n");

#if KHEAP_DEBUG
    kheap_selftest();
#endif
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;

    if (in_interrupt()) {
        kheap_panic("ILLEGAL: kmalloc called from interrupt context / ISR", 0, (uint64_t)size, 0);
    }

    size = (size + 15) & ~((size_t)15);

    uint64_t t0 = rdtsc_cycles();
    uint64_t flags = irq_save();

    block_header_t* block = heap_head;
    block_header_t* last = 0;
    int walk_count = 0;

    while (block) {
        if (++walk_count > 1024) {
            /* Bound the free-list walk to avoid long interrupt blackout */
            break;
        }
        kheap_verify_block(block);
        if (block->free && block->size >= size) {
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
            uint64_t dt = rdtsc_cycles() - t0;
            if (dt > g_kheap_max_kmalloc_cycles) {
                g_kheap_max_kmalloc_cycles = dt;
            }
            irq_restore(flags);
            return (void*)((uint8_t*)block + sizeof(block_header_t));
        }
        last = block;
        block = block->next;
    }

    block = kheap_expand(size, last);
    if (!block) {
        uint64_t dt = rdtsc_cycles() - t0;
        if (dt > g_kheap_max_kmalloc_cycles) {
            g_kheap_max_kmalloc_cycles = dt;
        }
        irq_restore(flags);
        return 0;
    }

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
    uint64_t dt = rdtsc_cycles() - t0;
    if (dt > g_kheap_max_kmalloc_cycles) {
        g_kheap_max_kmalloc_cycles = dt;
    }
    irq_restore(flags);
    return (void*)((uint8_t*)block + sizeof(block_header_t));
}

void kfree(void* ptr) {
    if (!ptr) return;

    if (in_interrupt()) {
        kheap_panic("ILLEGAL: kfree called from interrupt context / ISR", ptr, 0, 0);
    }

    uintptr_t v = (uintptr_t) ptr;
    if ((v & 15) != 0 || v < PHYSMAP_BASE) {
        kheap_panic("invalid pointer passed to kfree", ptr, v, 0);
    }

    uint64_t t0 = rdtsc_cycles();
    uint64_t flags = irq_save();

    block_header_t* block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));

    if (block->magic != KHEAP_MAGIC_HEAD) {
        irq_restore(flags);
        kheap_panic("corrupted block header magic (underflow or invalid ptr)", ptr, block->magic, KHEAP_MAGIC_HEAD);
    }

    if (block->free) {
        irq_restore(flags);
        kheap_panic("double free detected", ptr, block->free, 0);
    }

    uint32_t* tail = (uint32_t*)((uint8_t*)block + sizeof(block_header_t) + block->size);
    if (*tail != KHEAP_MAGIC_TAIL) {
        irq_restore(flags);
        kheap_panic("buffer overflow detected on free", ptr, *tail, KHEAP_MAGIC_TAIL);
    }

    *tail = 0;
    block->free = 1;
    block->req_size = 0;

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

    uint64_t dt = rdtsc_cycles() - t0;
    if (dt > g_kheap_max_kfree_cycles) {
        g_kheap_max_kfree_cycles = dt;
    }
    irq_restore(flags);
}

#if KHEAP_DEBUG
void kheap_selftest(void) {
    kprintf("[kheap] Running kernel heap hardening self-test...\n");
    serial_printf("[kheap] Running kernel heap hardening self-test...\n");

    #define TEST_BLOCKS 64
    void* ptrs[TEST_BLOCKS];
    size_t sizes[TEST_BLOCKS];

    for (int i = 0; i < TEST_BLOCKS; i++) {
        sizes[i] = ((i % 16) + 1) * 32;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            kheap_panic("selftest: kmalloc failed", (void*)(uintptr_t)i, sizes[i], 0);
        }
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            p[j] = (uint8_t)((i * 17 + j) ^ 0x5A);
        }
    }

    for (int i = 0; i < TEST_BLOCKS; i++) {
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)((i * 17 + j) ^ 0x5A)) {
                kheap_panic("selftest: pattern mismatch", ptrs[i], p[j], (i * 17 + j) ^ 0x5A);
            }
        }
    }

    for (int i = 0; i < TEST_BLOCKS; i += 2) {
        kfree(ptrs[i]);
    }

    for (int i = 0; i < TEST_BLOCKS; i += 2) {
        ptrs[i] = kmalloc(sizes[i] + 16);
        if (!ptrs[i]) {
            kheap_panic("selftest: realloc failed", (void*)(uintptr_t)i, sizes[i] + 16, 0);
        }
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < sizes[i] + 16; j++) {
            p[j] = (uint8_t)((i * 19 + j) ^ 0x3C);
        }
    }

    for (int i = 0; i < TEST_BLOCKS; i++) {
        kfree(ptrs[i]);
    }

    kprintf("[ok] Kernel heap self-test passed (canaries, coalescing, dynamic growth)\n");
    serial_printf("[ok] Kernel heap self-test passed (canaries, coalescing, dynamic growth)\n");
}
#endif

void* memset(void* dest, int c, size_t n) {
    uint8_t* d = (uint8_t*) dest;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t) c;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*) dest;
    const uint8_t* s = (const uint8_t*) src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*) dest;
    const uint8_t* s = (const uint8_t*) src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

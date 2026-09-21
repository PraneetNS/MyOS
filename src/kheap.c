#include "kheap.h"
#include "vga.h"
#include <stdint.h>

#define HEAP_SIZE (2 * 1024 * 1024)  /* 2MB kernel heap, comfortably inside our 16MB identity map */

/* Static backing store -- part of the kernel's own .bss, so it's already
   covered by the pmm's "reserve up to kernel_end" logic and by our
   identity-mapped paging. No dynamic page mapping needed for this stage. */
static uint8_t heap_region[HEAP_SIZE] __attribute__((aligned(16)));

typedef struct block_header {
    size_t size;              /* size of the usable region that follows this header */
    int free;
    struct block_header* next; /* next block by address order, or NULL at end */
} block_header_t;

static block_header_t* heap_start = 0;

void kheap_init(void) {
    heap_start = (block_header_t*) heap_region;
    heap_start->size = HEAP_SIZE - sizeof(block_header_t);
    heap_start->free = 1;
    heap_start->next = 0;

    terminal_writestring("[ok] Kernel heap initialized (2MB)\n");
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;

    /* align allocations to 16 bytes */
    size = (size + 15) & ~((size_t)15);

    block_header_t* block = heap_start;
    while (block) {
        if (block->free && block->size >= size) {
            /* split the block if there's enough room left over to be useful */
            size_t remaining = block->size - size;
            if (remaining > sizeof(block_header_t) + 16) {
                block_header_t* new_block =
                    (block_header_t*)((uint8_t*)block + sizeof(block_header_t) + size);
                new_block->size = remaining - sizeof(block_header_t);
                new_block->free = 1;
                new_block->next = block->next;

                block->size = size;
                block->next = new_block;
            }
            block->free = 0;
            return (uint8_t*)block + sizeof(block_header_t);
        }
        block = block->next;
    }

    return 0; /* heap exhausted */
}

void kfree(void* ptr) {
    if (!ptr) return;

    block_header_t* block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    block->free = 1;

    /* merge with the next block if it's also free and physically adjacent
       (true here since blocks are always carved forward from their parent) */
    if (block->next && block->next->free) {
        block->size += sizeof(block_header_t) + block->next->size;
        block->next = block->next->next;
    }
}

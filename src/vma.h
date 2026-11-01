#ifndef _VMA_H
#define _VMA_H

#include <stdint.h>
#include <stddef.h>

#define VMA_PROT_READ    0x1
#define VMA_PROT_WRITE   0x2
#define VMA_PROT_EXEC    0x4

#define VMA_FLAG_ANON    0x01
#define VMA_FLAG_FILE    0x02
#define VMA_FLAG_STACK   0x04
#define VMA_FLAG_HEAP    0x08
#define VMA_FLAG_DEVICE  0x10
#define VMA_FLAG_SHARED  0x20

struct vnode;
struct process;

typedef struct vma {
    uint64_t start;       /* Inclusive, page-aligned virtual address */
    uint64_t end;         /* Exclusive, page-aligned virtual address */
    uint32_t prot;        /* VMA_PROT_* */
    uint32_t flags;       /* VMA_FLAG_* */
    struct vnode* file;   /* Backing vnode (or NULL) */
    uint64_t offset;      /* Offset in file */
    uint64_t file_size;   /* Size of file data in segment */
    struct vma* next;
} vma_t;

vma_t* vma_create(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                  struct vnode* file, uint64_t offset, uint64_t file_size);
int vma_insert(vma_t** list_head, vma_t* vma);
vma_t* vma_find(struct process* proc, uint64_t addr);
vma_t* vma_clone_list(vma_t* head);
void vma_free_list(vma_t* head);
int vma_overlaps(struct process* proc, uint64_t start, uint64_t end);
uint64_t vma_find_free_gap(struct process* proc, uint64_t len);
int vma_unmap_range(struct process* proc, uint64_t start, uint64_t len);
int vma_mprotect(struct process* proc, uint64_t start, uint64_t len, int prot);

#endif

#include "vma.h"
#include "kheap.h"
#include "vfs.h"
#include "process.h"

vma_t* vma_create(uint32_t start, uint32_t end, uint32_t prot, uint32_t flags,
                  struct vnode* file, uint32_t offset, uint32_t file_size) {
    vma_t* vma = (vma_t*) kmalloc(sizeof(vma_t));
    if (!vma) return NULL;

    vma->start = start;
    vma->end = end;
    vma->prot = prot;
    vma->flags = flags;
    vma->file = file;
    vma->offset = offset;
    vma->file_size = file_size;
    vma->next = NULL;

    if (vma->file) {
        vnode_ref(vma->file);
    }

    return vma;
}

int vma_insert(vma_t** list_head, vma_t* vma) {
    if (!list_head || !vma) return -1;

    vma_t* prev = NULL;
    vma_t* curr = *list_head;

    while (curr && curr->start < vma->start) {
        prev = curr;
        curr = curr->next;
    }

    /* Check for overlap with prev */
    if (prev && prev->end > vma->start) {
        return -1;
    }

    /* Check for overlap with curr */
    if (curr && vma->end > curr->start) {
        return -1;
    }

    if (!prev) {
        vma->next = *list_head;
        *list_head = vma;
    } else {
        vma->next = prev->next;
        prev->next = vma;
    }

    return 0;
}

vma_t* vma_find(process_t* proc, uint32_t addr) {
    if (!proc) return NULL;
    vma_t* curr = proc->vma_list;
    while (curr) {
        if (curr->start <= addr && addr < curr->end) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

vma_t* vma_clone_list(vma_t* head) {
    vma_t* new_head = NULL;
    vma_t* tail = NULL;

    vma_t* curr = head;
    while (curr) {
        vma_t* copy = vma_create(curr->start, curr->end, curr->prot, curr->flags,
                                 curr->file, curr->offset, curr->file_size);
        if (!copy) {
            vma_free_list(new_head);
            return NULL;
        }
        if (!new_head) {
            new_head = copy;
            tail = copy;
        } else {
            tail->next = copy;
            tail = copy;
        }
        curr = curr->next;
    }
    return new_head;
}

void vma_free_list(vma_t* head) {
    vma_t* curr = head;
    while (curr) {
        vma_t* next = curr->next;
        if (curr->file) {
            vnode_unref(curr->file);
        }
        kfree(curr);
        curr = next;
    }
}

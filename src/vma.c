#include "vma.h"
#include "kheap.h"
#include "vfs.h"
#include "process.h"
#include "vmm.h"
#include "paging.h"
#include "errno.h"

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

int vma_overlaps(process_t* proc, uint32_t start, uint32_t end) {
    if (!proc) return 0;
    vma_t* curr = proc->vma_list;
    while (curr) {
        if (curr->start < end && curr->end > start) return 1;
        curr = curr->next;
    }
    return 0;
}

#define MMAP_BOTTOM_LIMIT 0x40000000u
#define MMAP_TOP_START    0xB8000000u

uint32_t vma_find_free_gap(process_t* proc, uint32_t len) {
    if (!proc || len == 0) return 0;
    if (len > (MMAP_TOP_START - MMAP_BOTTOM_LIMIT)) return 0;

    uint32_t best_addr = 0;
    uint32_t cur_bottom = MMAP_BOTTOM_LIMIT;

    vma_t* curr = proc->vma_list;
    while (curr) {
        if (curr->end <= cur_bottom) {
            curr = curr->next;
            continue;
        }

        if (curr->start > cur_bottom) {
            uint32_t gap_end = (curr->start < MMAP_TOP_START) ? curr->start : MMAP_TOP_START;
            if (gap_end > cur_bottom && (gap_end - cur_bottom) >= len) {
                uint32_t candidate = gap_end - len;
                if (candidate > best_addr) {
                    best_addr = candidate;
                }
            }
        }

        if (curr->start >= MMAP_TOP_START) {
            break;
        }

        if (curr->end > cur_bottom) {
            cur_bottom = curr->end;
        }
        if (cur_bottom >= MMAP_TOP_START) {
            break;
        }

        curr = curr->next;
    }

    if (cur_bottom < MMAP_TOP_START && (MMAP_TOP_START - cur_bottom) >= len) {
        uint32_t candidate = MMAP_TOP_START - len;
        if (candidate > best_addr) {
            best_addr = candidate;
        }
    }

    return best_addr;
}

int vma_unmap_range(process_t* proc, uint32_t start, uint32_t len) {
    if (!proc || len == 0) return -EINVAL;
    if (start & 0xFFFu) return -EINVAL;
    len = (len + 4095) & ~4095u;
    uint32_t end = start + len;
    if (end < start || end > 0xC0000000 || start < 0x1000) return -EINVAL;

    /* 1. Unmap pages in address space */
    for (uint32_t va = start; va < end; va += 4096) {
        vmm_unmap_user_page(&proc->as, va);
    }

    /* 2. Truncate / split / remove overlapping VMAs */
    vma_t** curr_ptr = &proc->vma_list;
    while (*curr_ptr) {
        vma_t* vma = *curr_ptr;
        if (vma->end <= start || vma->start >= end) {
            curr_ptr = &vma->next;
            continue;
        }

        if (start <= vma->start && end >= vma->end) {
            /* Case 1: completely contained in [start, end) */
            *curr_ptr = vma->next;
            if (vma->file) vnode_unref(vma->file);
            kfree(vma);
            continue;
        }

        if (vma->start < start && vma->end <= end) {
            /* Case 2: right side cut off */
            vma->end = start;
            if (vma->file_size > (vma->end - vma->start)) {
                vma->file_size = vma->end - vma->start;
            }
            curr_ptr = &vma->next;
            continue;
        }

        if (vma->start >= start && vma->end > end) {
            /* Case 3: left side cut off */
            uint32_t shift = end - vma->start;
            vma->start = end;
            vma->offset += shift;
            if (vma->file_size > shift) {
                vma->file_size -= shift;
            } else {
                vma->file_size = 0;
            }
            curr_ptr = &vma->next;
            continue;
        }

        if (vma->start < start && vma->end > end) {
            /* Case 4: middle cut out -> split into two VMAs */
            uint32_t orig_end = vma->end;
            uint32_t shift = end - vma->start;
            uint32_t r_offset = vma->offset + shift;
            uint32_t r_fsize = (vma->file_size > shift) ? (vma->file_size - shift) : 0;

            vma->end = start;
            if (vma->file_size > (vma->end - vma->start)) {
                vma->file_size = vma->end - vma->start;
            }

            vma_t* right = vma_create(end, orig_end, vma->prot, vma->flags,
                                      vma->file, r_offset, r_fsize);
            if (!right) {
                return -ENOMEM;
            }
            right->next = vma->next;
            vma->next = right;
            curr_ptr = &right->next;
            continue;
        }
    }

    return 0;
}

int vma_mprotect(process_t* proc, uint32_t start, uint32_t len, int prot) {
    if (!proc || len == 0) return -EINVAL;
    if (start & 0xFFFu) return -EINVAL;
    len = (len + 4095) & ~4095u;
    uint32_t end = start + len;
    if (end < start || end > 0xC0000000 || start < 0x1000) return -EINVAL;

    /* Verify that every address in [start, end) is backed by a VMA */
    uint32_t check = start;
    while (check < end) {
        vma_t* v = vma_find(proc, check);
        if (!v) return -ENOMEM;
        check = v->end;
    }

    uint32_t vprot = 0;
    if (prot & 0x1) vprot |= VMA_PROT_READ;
    if (prot & 0x2) vprot |= VMA_PROT_WRITE;
    if (prot & 0x4) vprot |= VMA_PROT_EXEC;

    /* Split boundary VMAs if needed */
    vma_t* curr = proc->vma_list;
    while (curr) {
        /* Split at start boundary */
        if (curr->start < start && curr->end > start) {
            uint32_t shift = start - curr->start;
            uint32_t r_offset = curr->offset + shift;
            uint32_t r_fsize = (curr->file_size > shift) ? (curr->file_size - shift) : 0;
            vma_t* right = vma_create(start, curr->end, curr->prot, curr->flags,
                                      curr->file, r_offset, r_fsize);
            if (!right) return -ENOMEM;
            curr->end = start;
            if (curr->file_size > (curr->end - curr->start)) {
                curr->file_size = curr->end - curr->start;
            }
            right->next = curr->next;
            curr->next = right;
            curr = right;
        }

        /* Split at end boundary */
        if (curr->start < end && curr->end > end) {
            uint32_t shift = end - curr->start;
            uint32_t r_offset = curr->offset + shift;
            uint32_t r_fsize = (curr->file_size > shift) ? (curr->file_size - shift) : 0;
            vma_t* right = vma_create(end, curr->end, curr->prot, curr->flags,
                                      curr->file, r_offset, r_fsize);
            if (!right) return -ENOMEM;
            curr->end = end;
            if (curr->file_size > (curr->end - curr->start)) {
                curr->file_size = curr->end - curr->start;
            }
            right->next = curr->next;
            curr->next = right;
            curr = right;
        }

        /* If strictly within [start, end), update prot */
        if (curr->start >= start && curr->end <= end) {
            curr->prot = vprot;
        }

        curr = curr->next;
    }

    /* Update page table permissions for existing mapped pages */
    for (uint32_t va = start; va < end; va += 4096) {
        uint32_t dir_idx = va >> 22;
        uint32_t tbl_idx = (va >> 12) & 0x3FF;
        if (proc->as.directory && (proc->as.directory[dir_idx] & PAGE_PRESENT)) {
            uint32_t* tbl = (uint32_t*) P2V(proc->as.directory[dir_idx] & ~0xFFFu);
            if (tbl[tbl_idx] & PAGE_PRESENT) {
                if (vprot & VMA_PROT_WRITE) {
                    if (!(tbl[tbl_idx] & PTE_COW)) {
                        tbl[tbl_idx] |= PAGE_WRITE;
                    }
                } else {
                    tbl[tbl_idx] &= ~PAGE_WRITE;
                }
                asm volatile ("invlpg (%0)" :: "r"(va) : "memory");
            }
        }
    }

    return 0;
}

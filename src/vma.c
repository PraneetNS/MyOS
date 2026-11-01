#include "vma.h"
#include "kheap.h"
#include "vfs.h"
#include "process.h"
#include "vmm.h"
#include "paging.h"
#include "errno.h"

vma_t* vma_create(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags,
                  struct vnode* file, uint64_t offset, uint64_t file_size) {
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

    if (prev && prev->end > vma->start) {
        return -1;
    }

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

vma_t* vma_find(process_t* proc, uint64_t addr) {
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
    vma_t** tail = &new_head;

    while (head) {
        vma_t* clone = vma_create(head->start, head->end, head->prot, head->flags,
                                  head->file, head->offset, head->file_size);
        if (!clone) {
            vma_free_list(new_head);
            return NULL;
        }
        *tail = clone;
        tail = &clone->next;
        head = head->next;
    }

    return new_head;
}

void vma_free_list(vma_t* head) {
    while (head) {
        vma_t* next = head->next;
        if (head->file) {
            vnode_unref(head->file);
        }
        kfree(head);
        head = next;
    }
}

int vma_overlaps(process_t* proc, uint64_t start, uint64_t end) {
    if (!proc) return 0;
    vma_t* curr = proc->vma_list;
    while (curr) {
        if (!(end <= curr->start || start >= curr->end)) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

#define MMAP_BOTTOM_LIMIT 0x0000700000000000ULL
#define MMAP_TOP_START    0x00007F0000000000ULL

uint64_t vma_find_free_gap(process_t* proc, uint64_t len) {
    if (!proc || len == 0) return 0;
    len = (len + 4095) & ~4095ULL;

    uint64_t mmap_top = MMAP_TOP_START;
    if (proc->aslr_mmap_offset > 0 && proc->aslr_mmap_offset < (MMAP_TOP_START - MMAP_BOTTOM_LIMIT - len)) {
        mmap_top -= proc->aslr_mmap_offset;
    }

    uint64_t cur_bottom = MMAP_BOTTOM_LIMIT;
    uint64_t best_addr = 0;

    vma_t* curr = proc->vma_list;
    while (curr) {
        if (curr->end <= MMAP_BOTTOM_LIMIT) {
            curr = curr->next;
            continue;
        }

        if (curr->start >= mmap_top) {
            break;
        }

        if (curr->start > cur_bottom) {
            uint64_t gap = curr->start - cur_bottom;
            if (gap >= len) {
                best_addr = curr->start - len;
            }
        }

        if (curr->end > cur_bottom) {
            cur_bottom = curr->end;
        }
        if (cur_bottom >= mmap_top) {
            break;
        }

        curr = curr->next;
    }

    if (cur_bottom < mmap_top && (mmap_top - cur_bottom) >= len) {
        uint64_t candidate = mmap_top - len;
        if (candidate > best_addr) {
            best_addr = candidate;
        }
    }

    return best_addr;
}

int vma_unmap_range(process_t* proc, uint64_t start, uint64_t len) {
    if (!proc || len == 0) return -EINVAL;
    if (start & 0xFFFULL) return -EINVAL;
    len = (len + 4095) & ~4095ULL;
    uint64_t end = start + len;
    if (end < start || end > 0x0000800000000000ULL || start < 0x1000) return -EINVAL;

    for (uint64_t va = start; va < end; va += 4096) {
        vmm_unmap_user_page(&proc->as, va);
    }

    vma_t** curr_ptr = &proc->vma_list;
    while (*curr_ptr) {
        vma_t* vma = *curr_ptr;
        if (vma->end <= start || vma->start >= end) {
            curr_ptr = &vma->next;
            continue;
        }

        if (start <= vma->start && end >= vma->end) {
            *curr_ptr = vma->next;
            if (vma->file) vnode_unref(vma->file);
            kfree(vma);
            continue;
        }

        if (vma->start < start && vma->end <= end) {
            vma->end = start;
            if (vma->file_size > (vma->end - vma->start)) {
                vma->file_size = vma->end - vma->start;
            }
            curr_ptr = &vma->next;
            continue;
        }

        if (vma->start >= start && vma->end > end) {
            uint64_t shift = end - vma->start;
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
            uint64_t orig_end = vma->end;
            uint64_t shift = end - vma->start;
            uint64_t r_offset = vma->offset + shift;
            uint64_t r_fsize = (vma->file_size > shift) ? (vma->file_size - shift) : 0;

            vma->end = start;
            if (vma->file_size > (vma->end - vma->start)) {
                vma->file_size = vma->end - vma->start;
            }

            vma_t* right = vma_create(end, orig_end, vma->prot, vma->flags,
                                      vma->file, r_offset, r_fsize);
            if (!right) return -ENOMEM;
            right->next = vma->next;
            vma->next = right;
            curr_ptr = &right->next;
            continue;
        }
    }

    return 0;
}

int vma_mprotect(process_t* proc, uint64_t start, uint64_t len, int prot) {
    if (!proc || len == 0) return -EINVAL;
    if (start & 0xFFFULL) return -EINVAL;
    len = (len + 4095) & ~4095ULL;
    uint64_t end = start + len;
    if (end < start || end > 0x0000800000000000ULL || start < 0x1000) return -EINVAL;

    uint64_t check = start;
    while (check < end) {
        vma_t* v = vma_find(proc, check);
        if (!v) return -ENOMEM;
        check = v->end;
    }

    uint32_t vprot = 0;
    if (prot & 0x1) vprot |= VMA_PROT_READ;
    if (prot & 0x2) vprot |= VMA_PROT_WRITE;
    if (prot & 0x4) vprot |= VMA_PROT_EXEC;

    vma_t** curr_ptr = &proc->vma_list;
    while (*curr_ptr) {
        vma_t* vma = *curr_ptr;
        if (vma->end <= start || vma->start >= end) {
            curr_ptr = &vma->next;
            continue;
        }

        if (start <= vma->start && end >= vma->end) {
            vma->prot = vprot;
            curr_ptr = &vma->next;
            continue;
        }

        if (vma->start < start && vma->end <= end) {
            uint64_t mid = start;
            uint64_t shift = mid - vma->start;
            uint64_t r_offset = vma->offset + shift;
            uint64_t r_fsize = (vma->file_size > shift) ? (vma->file_size - shift) : 0;

            uint64_t orig_end = vma->end;
            vma->end = mid;
            if (vma->file_size > shift) vma->file_size = shift;

            vma_t* right = vma_create(mid, orig_end, vprot, vma->flags, vma->file, r_offset, r_fsize);
            if (!right) return -ENOMEM;
            right->next = vma->next;
            vma->next = right;
            curr_ptr = &right->next;
            continue;
        }

        if (vma->start >= start && vma->end > end) {
            uint64_t mid = end;
            uint64_t shift = mid - vma->start;
            uint64_t r_offset = vma->offset + shift;
            uint64_t r_fsize = (vma->file_size > shift) ? (vma->file_size - shift) : 0;

            uint64_t orig_end = vma->end;
            vma->end = mid;
            vma->prot = vprot;

            vma_t* right = vma_create(mid, orig_end, vma->prot, vma->flags, vma->file, r_offset, r_fsize);
            if (!right) return -ENOMEM;
            right->next = vma->next;
            vma->next = right;
            curr_ptr = &right->next;
            continue;
        }

        if (vma->start < start && vma->end > end) {
            uint64_t mid1 = start;
            uint64_t mid2 = end;

            uint64_t orig_end = vma->end;
            uint64_t orig_prot = vma->prot;
            uint64_t orig_flags = vma->flags;
            struct vnode* f = vma->file;

            uint64_t shift1 = mid1 - vma->start;
            uint64_t shift2 = mid2 - vma->start;

            vma->end = mid1;
            if (vma->file_size > shift1) vma->file_size = shift1;

            vma_t* mid_vma = vma_create(mid1, mid2, vprot, orig_flags, f, vma->offset + shift1,
                                        (vma->file_size > shift2 ? (shift2 - shift1) : 0));
            vma_t* right_vma = vma_create(mid2, orig_end, orig_prot, orig_flags, f, vma->offset + shift2,
                                         (vma->file_size > shift2 ? (vma->file_size - shift2) : 0));
            if (!mid_vma || !right_vma) return -ENOMEM;

            right_vma->next = vma->next;
            mid_vma->next = right_vma;
            vma->next = mid_vma;
            curr_ptr = &right_vma->next;
            continue;
        }
    }

    return 0;
}

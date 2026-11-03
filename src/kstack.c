#include "kstack.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "serial.h"
#include "string.h"

static uint64_t* get_or_alloc_table(uint64_t* entry) {
    if (!(*entry & PAGE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return NULL;
        uint64_t* p = (uint64_t*) P2V(frame);
        for (int i = 0; i < 512; i++) p[i] = 0;
        *entry = frame | PAGE_PRESENT | PAGE_WRITE;
    }
    return (uint64_t*) P2V(*entry & ~0xFFFULL);
}

void kstack_init(void) {
    /* Virtual region 0xFFFFFFFFB0000000..0xFFFFFFFFB8000000 is reserved for kstack */
    serial_printf("[kstack] Initialized kernel stack allocator with guard pages (area: 0x%016llx)\n",
                  (unsigned long long)KSTACK_AREA_BASE);
}

int kstack_alloc(int slot, uint8_t** out_base, uint64_t* out_top) {
    if (slot < 0 || slot >= MAX_PROCESSES) return -1;

    uint64_t slot_base = KSTACK_AREA_BASE + (uint64_t)slot * KSTACK_SLOT_SIZE;
    uint64_t stack_base = slot_base + KSTACK_GUARD_SIZE;
    uint64_t num_pages = PROC_KERNEL_STACK_SIZE / 4096ULL;
    uint64_t* master_pml4 = paging_get_kernel_pml4_virt();

    /* 1. Ensure the guard page is unmapped */
    uint64_t pml4_idx = (slot_base >> 39) & 0x1FFULL;
    uint64_t pdpt_idx = (slot_base >> 30) & 0x1FFULL;
    uint64_t pd_idx   = (slot_base >> 21) & 0x1FFULL;
    uint64_t pt_idx   = (slot_base >> 12) & 0x1FFULL;

    uint64_t* pdpt = get_or_alloc_table(&master_pml4[pml4_idx]);
    if (!pdpt) return -1;
    uint64_t* pd = get_or_alloc_table(&pdpt[pdpt_idx]);
    if (!pd) return -1;
    uint64_t* pt = get_or_alloc_table(&pd[pd_idx]);
    if (!pt) return -1;

    pt[pt_idx] = 0; /* Guard page: unmapped */
    invlpg_page(slot_base);

    /* 2. Allocate and map stack pages */
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_va = stack_base + i * 4096ULL;
        uint64_t p_idx = (page_va >> 39) & 0x1FFULL;
        uint64_t d_idx = (page_va >> 30) & 0x1FFULL;
        uint64_t c_idx = (page_va >> 21) & 0x1FFULL;
        uint64_t t_idx = (page_va >> 12) & 0x1FFULL;

        uint64_t* p_pdpt = get_or_alloc_table(&master_pml4[p_idx]);
        if (!p_pdpt) return -1;
        uint64_t* p_pd = get_or_alloc_table(&p_pdpt[d_idx]);
        if (!p_pd) return -1;
        uint64_t* p_pt = get_or_alloc_table(&p_pd[c_idx]);
        if (!p_pt) return -1;

        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            /* Cleanup already allocated frames in this slot */
            for (uint64_t j = 0; j < i; j++) {
                uint64_t clean_va = stack_base + j * 4096ULL;
                uint64_t clean_t_idx = (clean_va >> 12) & 0x1FFULL;
                uint64_t f = p_pt[clean_t_idx] & ~0xFFFULL;
                if (f) pmm_free_frame(f);
                p_pt[clean_t_idx] = 0;
                invlpg_page(clean_va);
            }
            return -1;
        }

        p_pt[t_idx] = frame | PAGE_PRESENT | PAGE_WRITE | PAGE_NX;
        invlpg_page(page_va);
    }

    /* 3. Paint stack with signature byte for high-water mark tracking */
    uint8_t* base_ptr = (uint8_t*)stack_base;
    for (size_t i = 0; i < PROC_KERNEL_STACK_SIZE; i++) {
        base_ptr[i] = KSTACK_PAINT_BYTE;
    }

    *out_base = base_ptr;
    *out_top  = stack_base + PROC_KERNEL_STACK_SIZE;
    return 0;
}

void kstack_free(int slot) {
    if (slot < 0 || slot >= MAX_PROCESSES) return;

    uint64_t slot_base = KSTACK_AREA_BASE + (uint64_t)slot * KSTACK_SLOT_SIZE;
    uint64_t stack_base = slot_base + KSTACK_GUARD_SIZE;
    uint64_t num_pages = PROC_KERNEL_STACK_SIZE / 4096ULL;
    uint64_t* master_pml4 = paging_get_kernel_pml4_virt();

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_va = stack_base + i * 4096ULL;
        uint64_t p_idx = (page_va >> 39) & 0x1FFULL;
        uint64_t d_idx = (page_va >> 30) & 0x1FFULL;
        uint64_t c_idx = (page_va >> 21) & 0x1FFULL;
        uint64_t t_idx = (page_va >> 12) & 0x1FFULL;

        if (!(master_pml4[p_idx] & PAGE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*) P2V(master_pml4[p_idx] & ~0xFFFULL);
        if (!(pdpt[d_idx] & PAGE_PRESENT)) continue;
        uint64_t* pd = (uint64_t*) P2V(pdpt[d_idx] & ~0xFFFULL);
        if (!(pd[c_idx] & PAGE_PRESENT)) continue;
        uint64_t* pt = (uint64_t*) P2V(pd[c_idx] & ~0xFFFULL);

        if (pt[t_idx] & PAGE_PRESENT) {
            uint64_t frame = pt[t_idx] & ~0xFFFULL & ~(1ULL << 63);
            if (frame) {
                pmm_free_frame(frame);
            }
            pt[t_idx] = 0;
            invlpg_page(page_va);
        }
    }
}

size_t kstack_get_hwm(process_t* p) {
    if (!p || !p->kernel_stack_base) return 0;
    uint8_t* ptr = p->kernel_stack_base;
    size_t unused = 0;
    while (unused < PROC_KERNEL_STACK_SIZE && ptr[unused] == KSTACK_PAINT_BYTE) {
        unused++;
    }
    return PROC_KERNEL_STACK_SIZE - unused;
}

int is_kstack_guard_fault(uint64_t fault_addr, process_t* p) {
    if (!p || !p->kernel_stack_base) return 0;
    uint64_t guard_start = (uint64_t)p->kernel_stack_base - KSTACK_GUARD_SIZE;
    uint64_t guard_end   = (uint64_t)p->kernel_stack_base;
    return (fault_addr >= guard_start && fault_addr < guard_end);
}

__attribute__((noinline, optimize("O0"))) void trigger_stack_overflow(volatile int d) {
    volatile char buf[2048];
    buf[0] = (char)d;
    trigger_stack_overflow(d + 1);
    buf[1] = (char)d;
}

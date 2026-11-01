#include "ioremap.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "scheduler.h"

static uint64_t ioremap_next_vaddr = IOREMAP_START;
static int pat_initialized = 0;

static inline void rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi) {
    asm volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) : "a"(leaf));
}

int cpu_has_pat(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    return (d & (1 << 16)) != 0;
}

void pat_init(void) {
    if (pat_initialized) return;
    if (!cpu_has_pat()) return;

    /* IA32_PAT MSR is 0x277.
     * Entry layout:
     * PA0: bits 0..7   (WB  = 0x06)
     * PA1: bits 8..15  (WT  = 0x04)
     * PA2: bits 16..23 (UC- = 0x07)
     * PA3: bits 24..31 (UC  = 0x00)
     * PA4: bits 32..39 -> Write-Combining (0x01)
     */
    uint32_t lo = 0, hi = 0;
    rdmsr(0x277, &lo, &hi);
    hi = (hi & ~0xFFu) | 0x01u;
    wrmsr(0x277, lo, hi);
    pat_initialized = 1;
}

void* ioremap(uint64_t phys_addr, size_t size, uint32_t flags) {
    if (size == 0) return NULL;

    /* In x86-64 long mode, physical addresses of RAM and MMIO below 16GB
       can also be accessed directly via P2V, but ioremap establishes dedicated
       caching attributes (e.g. Write-Combining for Framebuffer or Uncached for BARs).
    */
    uint64_t offset = phys_addr & 0xFFFULL;
    uint64_t phys_base = phys_addr & ~0xFFFULL;
    uint64_t total_size = ((size + offset + 4095ULL) & ~4095ULL);
    uint64_t num_pages = total_size / 4096ULL;

    if (ioremap_next_vaddr + total_size > IOREMAP_END) {
        serial_printf("[ioremap] Out of virtual address space in higher canonical half\n");
        return NULL;
    }

    uint64_t vaddr = ioremap_next_vaddr;
    ioremap_next_vaddr += total_size;

    uint64_t cache_bits = 0;
    if (flags & IOMAP_WRITECOMBINING) {
        if (cpu_has_pat()) {
            pat_init();
            cache_bits = (1ULL << 7); /* PAT bit in 4KB PTE */
        } else {
            cache_bits = PAGE_PCD;
        }
    } else if (flags & IOMAP_NOCACHE) {
        cache_bits = PAGE_PCD;
    }

    uint64_t* kernel_pml4 = paging_get_kernel_pml4_virt();

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_va = vaddr + i * 4096ULL;
        uint64_t page_pa = phys_base + i * 4096ULL;

        uint64_t pml4_idx = (page_va >> 39) & 0x1FFULL;
        uint64_t pdpt_idx = (page_va >> 30) & 0x1FFULL;
        uint64_t pd_idx   = (page_va >> 21) & 0x1FFULL;
        uint64_t pt_idx   = (page_va >> 12) & 0x1FFULL;

        /* Allocate kernel PDPT / PD / PT tables if missing */
        if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
            uint64_t f = pmm_alloc_frame();
            if (!f) return NULL;
            uint64_t* p = (uint64_t*) P2V(f);
            for (int k = 0; k < 512; k++) p[k] = 0;
            kernel_pml4[pml4_idx] = f | PAGE_PRESENT | PAGE_WRITE;
        }
        uint64_t* pdpt = (uint64_t*) P2V(kernel_pml4[pml4_idx] & ~0xFFFULL);

        if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
            uint64_t f = pmm_alloc_frame();
            if (!f) return NULL;
            uint64_t* p = (uint64_t*) P2V(f);
            for (int k = 0; k < 512; k++) p[k] = 0;
            pdpt[pdpt_idx] = f | PAGE_PRESENT | PAGE_WRITE;
        }
        uint64_t* pd = (uint64_t*) P2V(pdpt[pdpt_idx] & ~0xFFFULL);

        if (!(pd[pd_idx] & PAGE_PRESENT)) {
            uint64_t f = pmm_alloc_frame();
            if (!f) return NULL;
            uint64_t* p = (uint64_t*) P2V(f);
            for (int k = 0; k < 512; k++) p[k] = 0;
            pd[pd_idx] = f | PAGE_PRESENT | PAGE_WRITE;
        }
        uint64_t* pt = (uint64_t*) P2V(pd[pd_idx] & ~0xFFFULL);

        pt[pt_idx] = page_pa | PAGE_PRESENT | PAGE_WRITE | cache_bits | PAGE_NX;
        invlpg_page(page_va);
    }

    return (void*)(vaddr + offset);
}

void iounmap(void* virt_addr, size_t size) {
    (void)virt_addr;
    (void)size;
}

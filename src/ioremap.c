#include "ioremap.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "scheduler.h"

static uint32_t ioremap_next_vaddr = IOREMAP_START;
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
     * PA4: bits 32..39 -> we configure to Write-Combining (0x01)
     * In 4KB PTE, PA4 is selected by setting PAT=1 (bit 7), PCD=0 (bit 4), PWT=0 (bit 3).
     */
    uint32_t lo = 0, hi = 0;
    rdmsr(0x277, &lo, &hi);
    hi = (hi & ~0xFFu) | 0x01u; /* PA4 = WC (0x01) */
    wrmsr(0x277, lo, hi);
    pat_initialized = 1;
}

void* ioremap(uint32_t phys_addr, size_t size, uint32_t flags) {
    if (size == 0) return NULL;

    uint32_t offset = phys_addr & 0xFFFu;
    uint32_t phys_base = phys_addr & ~0xFFFu;
    uint32_t total_size = ((size + offset + 4095u) & ~4095u);
    uint32_t num_pages = total_size / 4096u;

    if (ioremap_next_vaddr + total_size > IOREMAP_END) {
        serial_printf("[ioremap] Out of virtual address space in 0xF0000000-0xFFBFFFFF\n");
        return NULL;
    }

    uint32_t vaddr = ioremap_next_vaddr;
    ioremap_next_vaddr += total_size;

    /* Determine caching bits for PTE */
    uint32_t cache_bits = 0;
    if (flags & IOMAP_WRITECOMBINING) {
        if (cpu_has_pat()) {
            pat_init();
            cache_bits = 0x80u; /* PAT bit in 4KB PTE selects PA4 (WC) */
        } else {
            cache_bits = 0x10u; /* PCD fallback (Uncached) */
        }
    } else if (flags & IOMAP_NOCACHE) {
        cache_bits = 0x10u;     /* PCD */
    }

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t page_va = vaddr + i * 4096u;
        uint32_t page_pa = phys_base + i * 4096u;
        uint32_t dir_idx = page_va >> 22;
        uint32_t tbl_idx = (page_va >> 12) & 0x3FFu;

        if (!(boot_page_directory[dir_idx] & PAGE_PRESENT)) {
            uint32_t pt_phys = pmm_alloc_frame();
            if (!pt_phys) {
                serial_printf("[ioremap] Failed to alloc frame for page table\n");
                return NULL;
            }
            uint32_t* pt_virt = (uint32_t*) P2V(pt_phys);
            for (int k = 0; k < 1024; k++) pt_virt[k] = 0;
            boot_page_directory[dir_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITE;

            process_t* cur = scheduler_current();
            if (cur && cur->as.directory) {
                cur->as.directory[dir_idx] = boot_page_directory[dir_idx];
            }
        }

        uint32_t* pt = (uint32_t*) P2V(boot_page_directory[dir_idx] & ~0xFFFu);
        pt[tbl_idx] = page_pa | PAGE_PRESENT | PAGE_WRITE | cache_bits;
        asm volatile("invlpg (%0)" :: "r"(page_va) : "memory");
    }

    return (void*)(vaddr + offset);
}

void iounmap(void* virt_addr, size_t size) {
    (void)virt_addr;
    (void)size;
}

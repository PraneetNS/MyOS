#include "fpu.h"
#include "serial.h"
#include "vga.h"
#include <string.h>

static int has_fpu = 0;
static int has_fxsr = 0;
static int has_sse = 0;
static int has_sse2 = 0;

static uint8_t default_fpu_state[512] __attribute__((aligned(16)));

int fpu_has_fpu(void) { return has_fpu; }
int fpu_has_fxsr(void) { return has_fxsr; }
int fpu_has_sse(void) { return has_sse; }
int fpu_has_sse2(void) { return has_sse2; }

static inline int has_cpuid_instruction(void) {
    uint32_t eflags1, eflags2;
    asm volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x00200000, %1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        : "=r"(eflags1), "=r"(eflags2)
    );
    return ((eflags1 ^ eflags2) & 0x00200000) != 0;
}

static inline void cpuid(uint32_t code, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    asm volatile (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(code)
    );
}

static inline uint8_t* process_fpu_state(process_t* p) {
    return (uint8_t*)(((uintptr_t)p->fpu_buffer + 15) & ~15u);
}

void fpu_init(void) {
    has_fpu = 0;
    has_fxsr = 0;
    has_sse = 0;
    has_sse2 = 0;

    if (has_cpuid_instruction()) {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpuid(1, &eax, &ebx, &ecx, &edx);
        has_fpu  = (edx & (1u << 0))  != 0;
        has_fxsr = (edx & (1u << 24)) != 0;
        has_sse  = (edx & (1u << 25)) != 0;
        has_sse2 = (edx & (1u << 26)) != 0;
    }

    /* Configure CR0 for x87 coprocessor */
    uint32_t cr0;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2); /* Clear EM (coprocessor emulation) */
    cr0 |=  (1u << 1); /* Set MP (monitor coprocessor) */
    cr0 &= ~(1u << 3); /* Clear TS (task switched) */
    cr0 |=  (1u << 5); /* Set NE (native x87 exception handling on vector 16) */
    asm volatile ("mov %0, %%cr0" : : "r"(cr0));

    if (has_fpu) {
        asm volatile ("fninit");
    }

    /* Configure CR4 for FXSR and SSE */
    if (has_fxsr) {
        uint32_t cr4;
        asm volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9);  /* OSFXSR: enable FXSAVE / FXRSTOR */
        cr4 |= (1u << 10); /* OSXMMEXCPT: enable SIMD exceptions (#XM vector 19) */
        asm volatile ("mov %0, %%cr4" : : "r"(cr4));
    }

    if (has_sse) {
        uint32_t mxcsr = 0x1F80; /* default MXCSR: mask all SIMD exceptions */
        asm volatile ("ldmxcsr %0" : : "m"(mxcsr));
    }

    memset(default_fpu_state, 0, sizeof(default_fpu_state));
    if (has_fxsr) {
        asm volatile ("fxsave (%0)" : : "r"(default_fpu_state) : "memory");
    } else if (has_fpu) {
        asm volatile ("fnsave (%0)" : : "r"(default_fpu_state) : "memory");
    }

    serial_printf("[fpu] Capabilities: FPU=%s, FXSR=%s, SSE=%s, SSE2=%s\n",
                  has_fpu ? "yes" : "no",
                  has_fxsr ? "yes" : "no",
                  has_sse ? "yes" : "no",
                  has_sse2 ? "yes" : "no");
    kprintf("[ok] FPU/SSE initialized (FPU:%s FXSR:%s SSE:%s SSE2:%s)\n",
            has_fpu ? "yes" : "no",
            has_fxsr ? "yes" : "no",
            has_sse ? "yes" : "no",
            has_sse2 ? "yes" : "no");
}

void fpu_init_proc(process_t* p) {
    if (!p) return;
    memcpy(process_fpu_state(p), default_fpu_state, 512);
}

void fpu_copy(process_t* dst, process_t* src) {
    if (!dst || !src) return;
    memcpy(process_fpu_state(dst), process_fpu_state(src), 512);
}

void fpu_save(process_t* p) {
    if (!p) return;
    uint8_t* area = process_fpu_state(p);
    if (has_fxsr) {
        asm volatile ("fxsave (%0)" : : "r"(area) : "memory");
    } else if (has_fpu) {
        asm volatile ("fnsave (%0)" : : "r"(area) : "memory");
    }
}

void fpu_restore(process_t* p) {
    if (!p) return;
    uint8_t* area = process_fpu_state(p);
    if (has_fxsr) {
        asm volatile ("fxrstor (%0)" : : "r"(area) : "memory");
    } else if (has_fpu) {
        asm volatile ("frstor (%0)" : : "r"(area) : "memory");
    }
}

void fpu_switch(process_t* prev, process_t* next) {
    if (prev == next) return;
    if (prev && !prev->is_kernel_task) {
        fpu_save(prev);
    }
    if (next && !next->is_kernel_task) {
        fpu_restore(next);
    }
}

void fpu_save_to(uint8_t* dst_512) {
    if (!dst_512) return;
    if (has_fxsr) {
        asm volatile ("fxsave (%0)" : : "r"(dst_512) : "memory");
    } else if (has_fpu) {
        asm volatile ("fnsave (%0)" : : "r"(dst_512) : "memory");
    }
}

void fpu_restore_from(const uint8_t* src_512) {
    if (!src_512) return;
    if (has_fxsr) {
        asm volatile ("fxrstor (%0)" : : "r"(src_512) : "memory");
    } else if (has_fpu) {
        asm volatile ("frstor (%0)" : : "r"(src_512) : "memory");
    }
}

void fpu_proc_set_state(process_t* p, const uint8_t* state_512) {
    if (!p || !state_512) return;
    memcpy(process_fpu_state(p), state_512, 512);
}

uint8_t* fpu_get_state_ptr(process_t* p) {
    if (!p) return 0;
    return process_fpu_state(p);
}

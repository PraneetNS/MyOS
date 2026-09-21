/* badwrite.c -- deliberately writes to kernel memory (address 0x100000,
   well inside the 0-4MB region that paging.c marks supervisor-only in
   every process's address space) to prove Stage 6's memory protection
   actually works: this should page-fault, get caught by the kernel's
   page fault handler, and the process gets terminated cleanly -- NOT
   silently corrupt kernel memory, and NOT crash the whole machine. */

static inline void sys_write(const char* s) {
    asm volatile ("int $0x80" : : "a"(1), "b"(s));
}

void _start(void) {
    sys_write("[badwrite] About to write to kernel memory at 0x100000...\n");
    sys_write("[badwrite] If memory protection works, you'll see a page\n");
    sys_write("[badwrite] fault message next, NOT this program continuing.\n");

    volatile unsigned int* kernel_addr = (volatile unsigned int*) 0x100000;
    *kernel_addr = 0xDEADBEEF; /* should fault -- this page is supervisor-only */

    /* Unreachable if protection works correctly. */
    sys_write("[badwrite] If you see this, memory protection FAILED.\n");
    for (;;) { }
}

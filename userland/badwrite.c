/* badwrite.c -- deliberately writes to kernel memory (address 0x100000,
   well inside the 0-4MB region that paging.c marks supervisor-only in
   every process's address space) to prove Stage 6's memory protection
   actually works: this should page-fault, get caught by the kernel's
   page fault handler, and the process gets terminated cleanly -- NOT
   silently corrupt kernel memory, and NOT crash the whole machine. */

#include "libc.h"

void _start(void) {
    const char* m1 = "[badwrite] About to write to kernel memory at 0x100000...\n";
    const char* m2 = "[badwrite] If memory protection works, you'll see a page\n";
    const char* m3 = "[badwrite] fault message next, NOT this program continuing.\n";
    sys_write(1, m1, strlen(m1));
    sys_write(1, m2, strlen(m2));
    sys_write(1, m3, strlen(m3));

    volatile unsigned int* kernel_addr = (volatile unsigned int*) 0x100000;
    *kernel_addr = 0xDEADBEEF; /* should fault -- this page is supervisor-only */

    /* Unreachable if protection works correctly. */
    const char* m4 = "[badwrite] If you see this, memory protection FAILED.\n";
    sys_write(1, m4, strlen(m4));
    for (;;) { }
}

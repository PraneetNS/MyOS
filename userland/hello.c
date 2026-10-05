/* hello.c -- a real, standalone ELF32 program. Compiled and linked
   completely separately from the kernel (see build.sh), then placed
   onto the disk image as a file the kernel loads at runtime. */

#include "libc.h"

void _start(void) {
    const char* m1 = "Hello from a REAL ELF binary, loaded from disk by MyOS!\n";
    const char* m2 = "This program was compiled separately, written to disk by\n";
    const char* m3 = "tools/build_disk.py, and just now: read via the ATA driver,\n";
    const char* m4 = "parsed by the ELF loader, and run in ring 3. \n";

    sys_write(1, m1, strlen(m1));
    sys_write(1, m2, strlen(m2));
    sys_write(1, m3, strlen(m3));
    sys_write(1, m4, strlen(m4));
    sys_exit();

    for (;;) { }
}

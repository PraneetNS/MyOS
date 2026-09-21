/* hello.c -- a real, standalone ELF32 program. Compiled and linked
   completely separately from the kernel (see build.sh), then placed
   onto the disk image as a file the kernel loads at runtime. */

#include "libc.h"

void _start(void) {
    sys_write("Hello from a REAL ELF binary, loaded from disk by MyOS!\n");
    sys_write("This program was compiled separately, written to disk by\n");
    sys_write("tools/build_disk.py, and just now: read via the ATA driver,\n");
    sys_write("parsed by the ELF loader, and run in ring 3. \n");
    sys_exit();

    for (;;) { }
}

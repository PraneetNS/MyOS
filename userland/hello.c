/* hello.c -- a real, standalone ELF32 program. Compiled and linked
   completely separately from the kernel (see build.sh), then placed
   onto the disk image as a file the kernel loads at runtime.

   It talks to the kernel ONLY through int 0x80, exactly like Stage 4's
   baked-in demo -- the difference is this binary now lives on disk as
   genuine ELF bytes that the kernel's elf.c parses at runtime, instead
   of being compiled directly into the kernel image. */

static inline void sys_write(const char* s) {
    asm volatile ("int $0x80" : : "a"(1), "b"(s));
}

static inline void sys_exit(void) {
    asm volatile ("int $0x80" : : "a"(0));
}

void _start(void) {
    sys_write("Hello from a REAL ELF binary, loaded from disk by MyOS!\n");
    sys_write("This program was compiled separately, written to disk by\n");
    sys_write("tools/build_disk.py, and just now: read via the ATA driver,\n");
    sys_write("parsed by the ELF loader, and run in ring 3. \n");
    sys_exit();

    for (;;) { }
}

/* heaptest.c -- proves sys_sbrk() provides real, usable, zero-initialized
   memory: grows the heap, writes actual data into the new region, reads
   it back, and grows it again to show the break keeps advancing. */

#include "libc.h"

void _start(void) {
    sys_write("[heaptest] pid ");
    print_uint((unsigned int) sys_getpid());
    sys_write(": querying initial heap break...\n");

    char* initial = (char*) sys_sbrk(0);
    sys_write("[heaptest] initial break: ");
    print_hex((unsigned int) initial);
    sys_write("\n");

    sys_write("[heaptest] growing heap by 4096 bytes...\n");
    char* region1 = (char*) sys_sbrk(4096);
    sys_write("[heaptest] sys_sbrk returned (start of new region): ");
    print_hex((unsigned int) region1);
    sys_write("\n");

    /* Prove it's real, writable, zero-initialized memory -- not just a
       returned number that would fault on use. */
    sys_write("[heaptest] first byte before writing (should be 0): ");
    print_uint((unsigned int)(unsigned char) region1[0]);
    sys_write("\n");

    const char* msg = "Hello from the heap!";
    int i = 0;
    while (msg[i]) { region1[i] = msg[i]; i++; }
    region1[i] = '\0';

    sys_write("[heaptest] wrote a string into the new heap region, reading it back: ");
    sys_write(region1);
    sys_write("\n");

    sys_write("[heaptest] growing heap by another 4096 bytes...\n");
    char* region2 = (char*) sys_sbrk(4096);
    sys_write("[heaptest] second region starts at: ");
    print_hex((unsigned int) region2);
    sys_write(" (should be exactly 4096 past the first)\n");

    region2[0] = 'X';
    sys_write("[heaptest] wrote to the second region too -- byte: ");
    print_uint((unsigned int)(unsigned char) region2[0]);
    sys_write("\n");

    sys_write("[heaptest] done, exiting.\n");
    sys_exit();

    for (;;) { }
}

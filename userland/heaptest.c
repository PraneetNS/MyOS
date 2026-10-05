/* heaptest.c -- proves sys_sbrk() provides real, usable, zero-initialized
   memory: grows the heap, writes actual data into the new region, reads
   it back, and grows it again to show the break keeps advancing. */

#include "libc.h"

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    const char* m1 = "[heaptest] pid ";
    const char* m2 = ": querying initial heap break...\n";
    sys_write(1, m1, strlen(m1));
    print_uint((unsigned int) sys_getpid());
    sys_write(1, m2, strlen(m2));

    char* initial = (char*) sys_sbrk(0);
    const char* m3 = "[heaptest] initial break: ";
    const char* nl = "\n";
    sys_write(1, m3, strlen(m3));
    print_hex((unsigned int) initial);
    sys_write(1, nl, 1);

    const char* m4 = "[heaptest] growing heap by 4096 bytes...\n";
    sys_write(1, m4, strlen(m4));
    char* region1 = (char*) sys_sbrk(4096);
    const char* m5 = "[heaptest] sys_sbrk returned (start of new region): ";
    sys_write(1, m5, strlen(m5));
    print_hex((unsigned int) region1);
    sys_write(1, nl, 1);

    /* Prove it's real, writable, zero-initialized memory -- not just a
       returned number that would fault on use. */
    const char* m6 = "[heaptest] first byte before writing (should be 0): ";
    sys_write(1, m6, strlen(m6));
    print_uint((unsigned int)(unsigned char) region1[0]);
    sys_write(1, nl, 1);

    const char* msg = "Hello from the heap!";
    int i = 0;
    while (msg[i]) { region1[i] = msg[i]; i++; }
    region1[i] = '\0';

    const char* m7 = "[heaptest] wrote a string into the new heap region, reading it back: ";
    sys_write(1, m7, strlen(m7));
    sys_write(1, region1, strlen(region1));
    sys_write(1, nl, 1);

    const char* m8 = "[heaptest] growing heap by another 4096 bytes...\n";
    sys_write(1, m8, strlen(m8));
    char* region2 = (char*) sys_sbrk(4096);
    const char* m9 = "[heaptest] second region starts at: ";
    const char* m10 = " (should be exactly 4096 past the first)\n";
    sys_write(1, m9, strlen(m9));
    print_hex((unsigned int) region2);
    sys_write(1, m10, strlen(m10));

    region2[0] = 'X';
    const char* m11 = "[heaptest] wrote to the second region too -- byte: ";
    sys_write(1, m11, strlen(m11));
    print_uint((unsigned int)(unsigned char) region2[0]);
    sys_write(1, nl, 1);

    const char* md = "[heaptest] done, exiting.\n";
    sys_write(1, md, strlen(md));
    sys_exit();

    for (;;) { }
}

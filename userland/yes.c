/* yes.c -- output a string repeatedly until killed or broken pipe */

#include "libc.h"

int main(int argc, char** argv) {
    const char* str = "y";
    if (argc >= 2) {
        str = argv[1];
    }

    int len = strlen(str);
    char buf[128];
    int blen = 0;
    while (blen < len && blen < 126) {
        buf[blen] = str[blen];
        blen++;
    }
    buf[blen++] = '\n';
    buf[blen] = '\0';

    for (;;) {
        int n = write(1, buf, blen);
        if (n < 0) {
            /* Broken pipe or error: terminate cleanly */
            break;
        }
    }

    exit(0);
    return 0;
}

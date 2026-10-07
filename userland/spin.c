#include "libc.h"

int main(void) {
    for (;;) {
        __asm__ __volatile__ ("");
    }
    return 0;
}

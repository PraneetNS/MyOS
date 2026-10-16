#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    printf("Halting system...\n");
    reboot(RB_HALT_SYSTEM);
    return 0;
}

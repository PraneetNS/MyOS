#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    printf("Rebooting system...\n");
    reboot(RB_AUTOBOOT);
    return 0;
}

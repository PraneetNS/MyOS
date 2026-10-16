#include <stdio.h>
#include <sys/reboot.h>

int main(int argc, char** argv) {
    (void) argc; (void) argv;
    printf("Powering off system...\n");
    reboot(RB_POWER_OFF);
    return 0;
}

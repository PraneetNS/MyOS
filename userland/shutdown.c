#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>

int main(int argc, char** argv) {
    int cmd = RB_POWER_OFF;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) cmd = RB_AUTOBOOT;
        else if (strcmp(argv[i], "-h") == 0) cmd = RB_HALT_SYSTEM;
        else if (strcmp(argv[i], "-P") == 0) cmd = RB_POWER_OFF;
    }
    printf("Shutting down system...\n");
    reboot(cmd);
    return 0;
}

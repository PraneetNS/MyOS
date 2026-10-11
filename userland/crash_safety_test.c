#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kd.h>

int main(void) {
    printf("[crash_safety] Switching console to KD_GRAPHICS and K_OFF...\n");
    int cfd = open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        ioctl(cfd, KDSETMODE, KD_GRAPHICS);
        ioctl(cfd, KDSKBMODE, K_OFF);
        close(cfd);
    }

    printf("[crash_safety] Triggering intentional page fault (segfault)...\n");
    volatile int *bad_ptr = (volatile int *) 0x0;
    *bad_ptr = 42;

    return 0;
}

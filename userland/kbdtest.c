#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kd.h>

int main(void) {
    printf("[kbdtest] Stage 18A Step 4: keyboard input layer test\n");

    /* 1. Open /dev/input/kbd0 */
    int fd = open("/dev/input/kbd0", O_RDONLY);
    if (fd < 0) {
        printf("[kbdtest] FAIL: open /dev/input/kbd0 failed (errno=%d)\n", errno);
        return 1;
    }
    printf("[kbdtest] PASS: /dev/input/kbd0 opened (fd=%d)\n", fd);

    /* 2. Stat /dev/input/kbd0 */
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        printf("[kbdtest] FAIL: /dev/input/kbd0 not a character device\n");
        close(fd);
        return 1;
    }
    printf("[kbdtest] PASS: /dev/input/kbd0 is a character device\n");

    /* 3. Query modifiers via ioctl */
    uint32_t mods = 0;
    if (ioctl(fd, KBD_IOC_GETMODS, &mods) != 0) {
        printf("[kbdtest] FAIL: KBD_IOC_GETMODS ioctl failed (errno=%d)\n", errno);
        close(fd);
        return 1;
    }
    printf("[kbdtest] PASS: KBD_IOC_GETMODS returned 0x%x\n", (unsigned int)mods);
    close(fd);

    /* 4. Test KDSKBMODE on /dev/console */
    int cfd = open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        int mode = 0;
        if (ioctl(cfd, KDGKBMODE, &mode) == 0) {
            printf("[kbdtest] Initial kbmode: %d (expected %d)\n", mode, K_XLATE);
        }

        if (ioctl(cfd, KDSKBMODE, K_OFF) != 0) {
            printf("[kbdtest] FAIL: KDSKBMODE K_OFF failed (errno=%d)\n", errno);
            close(cfd);
            return 1;
        }

        if (ioctl(cfd, KDGKBMODE, &mode) == 0 && mode == K_OFF) {
            printf("[kbdtest] PASS: KDSKBMODE set K_OFF verified\n");
        } else {
            printf("[kbdtest] FAIL: KDGKBMODE did not return K_OFF (mode=%d)\n", mode);
            close(cfd);
            return 1;
        }

        /* Restore K_XLATE */
        ioctl(cfd, KDSKBMODE, K_XLATE);
        ioctl(cfd, KDGKBMODE, &mode);
        if (mode == K_XLATE) {
            printf("[kbdtest] PASS: Restored K_XLATE\n");
        }
        close(cfd);
    } else {
        printf("[kbdtest] WARN: open /dev/console failed (errno=%d)\n", errno);
    }

    printf("[kbdtest] ALL KEYBOARD TESTS PASSED\n");
    return 0;
}

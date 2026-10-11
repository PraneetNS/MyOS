#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kd.h>

int main(int argc, char *argv[]) {
    int events_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--events") == 0) {
            events_mode = 1;
        }
    }

    if (events_mode) {
        printf("[kbdtest] Testing raw keyboard injection in K_OFF mode...\n");
        int cfd = open("/dev/console", O_RDWR);
        if (cfd < 0) {
            printf("[kbdtest] FAIL: open /dev/console failed (errno=%d)\n", errno);
            return 1;
        }
        if (ioctl(cfd, KDSKBMODE, K_OFF) != 0) {
            printf("[kbdtest] FAIL: KDSKBMODE K_OFF failed (errno=%d)\n", errno);
            close(cfd);
            return 1;
        }

        int kfd = open("/dev/input/kbd0", O_RDONLY);
        if (kfd < 0) {
            printf("[kbdtest] FAIL: open /dev/input/kbd0 failed (errno=%d)\n", errno);
            ioctl(cfd, KDSKBMODE, K_XLATE);
            close(cfd);
            return 1;
        }

        printf("[kbdtest] Waiting for key 'a' events on /dev/input/kbd0...\n");
        int got_press = 0, got_release = 0;
        int elapsed_ms = 0;
        while (elapsed_ms < 6000) {
            struct pollfd pfd = { .fd = kfd, .events = POLLIN, .revents = 0 };
            int ret = poll(&pfd, 1, 200);
            elapsed_ms += 200;
            if (ret > 0 && (pfd.revents & POLLIN)) {
                struct input_event ev;
                int n = read(kfd, &ev, sizeof(ev));
                if (n == (int)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == 30) {
                        if (ev.value == 1) {
                            printf("[kbdtest] EVENT: keycode=%d value=%d (press)\n", ev.code, ev.value);
                            got_press = 1;
                        } else if (ev.value == 0) {
                            printf("[kbdtest] EVENT: keycode=%d value=%d (release)\n", ev.code, ev.value);
                            got_release = 1;
                        }
                        if (got_press && got_release) {
                            break;
                        }
                    }
                }
            }
        }

        close(kfd);
        ioctl(cfd, KDSKBMODE, K_XLATE);
        close(cfd);
        printf("[kbdtest] Keyboard events received: press=%d release=%d\n", got_press, got_release);
        return (got_press && got_release) ? 0 : 1;
    }

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

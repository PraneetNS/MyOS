#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/kd.h>

static unsigned int get_free_frames(void) {
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    const char* key = "FramesFree";
    char* p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    unsigned int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

int main(void) {
    printf("[fbtest] Starting fb0 and device mmap test...\n");

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        printf("[fbtest] FAIL: open /dev/fb0 failed\n");
        return 1;
    }

    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0) {
        printf("[fbtest] FAIL: FBIOGET_VSCREENINFO failed\n");
        close(fd);
        return 1;
    }

    printf("[fbtest] Screen info: %ux%u, %u bpp, pitch=%u\n",
           vi.xres, vi.yres, vi.bits_per_pixel, vi.line_length);

    if (vi.xres != 1024 || vi.yres != 768 || vi.bits_per_pixel != 32) {
        printf("[fbtest] FAIL: unexpected dimensions/bpp\n");
        close(fd);
        return 1;
    }

    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        printf("[fbtest] FAIL: FBIOGET_FSCREENINFO failed\n");
        close(fd);
        return 1;
    }

    printf("[fbtest] Fixed info: smem_len=%u\n", fi.smem_len);
    if (fi.smem_len < vi.line_length * vi.yres) {
        printf("[fbtest] FAIL: smem_len too small\n");
        close(fd);
        return 1;
    }

    /* Test write() and lseek() */
    off_t pos = lseek(fd, 0, SEEK_SET);
    if (pos != 0) {
        printf("[fbtest] FAIL: lseek failed\n");
        close(fd);
        return 1;
    }
    uint32_t sample_pixel = 0x00FF0000; /* Red */
    if (write(fd, &sample_pixel, 4) != 4) {
        printf("[fbtest] FAIL: write failed\n");
        close(fd);
        return 1;
    }

    /* Get free frames baseline */
    unsigned int frames_before = get_free_frames();
    printf("[fbtest] Frames free baseline: %u\n", frames_before);

    /* mmap fb0 */
    volatile uint32_t* fb_ptr = (volatile uint32_t*) mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_ptr == MAP_FAILED) {
        printf("[fbtest] FAIL: mmap failed\n");
        close(fd);
        return 1;
    }

    int cfd = open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        ioctl(cfd, KDSETMODE, KD_GRAPHICS);
    }

    uint32_t test_idx1 = vi.xres * 500 + 10;
    uint32_t test_idx2 = vi.xres * 500 + 20;
    uint32_t test_idx3 = vi.xres * 500 + 30;

    printf("[fbtest] fb_ptr=%p, &test_idx3=%p\n", (void*)fb_ptr, (void*)&fb_ptr[test_idx3]);

    fb_ptr[test_idx1] = 0x00112233;
    fb_ptr[test_idx2] = 0x00445566;

    /* Fork test: parent and child share device mapping without COW */
    pid_t pid = fork();
    if (pid < 0) {
        printf("[fbtest] FAIL: fork failed\n");
        munmap((void*)fb_ptr, fi.smem_len);
        if (cfd >= 0) { ioctl(cfd, KDSETMODE, KD_TEXT); close(cfd); }
        close(fd);
        return 1;
    }

    if (pid == 0) {
        /* Child */
        if (fb_ptr[test_idx1] != 0x00112233) {
            printf("[fbtest-child] FAIL: value mismatch at test_idx1: 0x%08x\n", fb_ptr[test_idx1]);
            exit(2);
        }
        fb_ptr[test_idx3] = 0x00778899;
        printf("[fbtest-child] wrote test_idx3, readback=0x%08x\n", fb_ptr[test_idx3]);
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0) {
        printf("[fbtest] FAIL: child exited with error %d\n", status);
        munmap((void*)fb_ptr, fi.smem_len);
        if (cfd >= 0) { ioctl(cfd, KDSETMODE, KD_TEXT); close(cfd); }
        close(fd);
        return 1;
    }

    printf("[fbtest] parent read test_idx3 = 0x%08x\n", fb_ptr[test_idx3]);
    if (fb_ptr[test_idx3] != 0x00778899) {
        printf("[fbtest] FAIL: child modification not seen in parent\n");
        munmap((void*)fb_ptr, fi.smem_len);
        if (cfd >= 0) { ioctl(cfd, KDSETMODE, KD_TEXT); close(cfd); }
        close(fd);
        return 1;
    }

    if (cfd >= 0) {
        ioctl(cfd, KDSETMODE, KD_TEXT);
        close(cfd);
    }

    if (munmap((void*)fb_ptr, fi.smem_len) != 0) {
        printf("[fbtest] FAIL: munmap failed\n");
        close(fd);
        return 1;
    }

    close(fd);

    unsigned int frames_after = get_free_frames();
    printf("[fbtest] Frames free after: %u\n", frames_after);
    if (frames_before != 0 && frames_after != 0) {
        /* Small variance might happen due to process structures if any, but device frames must not leak */
        if (frames_after + 5 < frames_before || frames_after > frames_before + 5) {
            printf("[fbtest] WARNING: frame count shifted: before=%u, after=%u\n", frames_before, frames_after);
        }
    }

    printf("[fbtest] PASS\n");
    return 0;
}

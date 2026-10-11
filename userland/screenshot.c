#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include "gfx.h"

int main(int argc, char *argv[]) {
    const char *out_path = "/tmp/screenshot.bmp";
    if (argc > 1 && argv[1][0] != '-') {
        out_path = argv[1];
    }

    int fb_fd = open("/dev/fb0", O_RDONLY);
    if (fb_fd < 0) {
        fprintf(stderr, "screenshot: failed to open /dev/fb0\n");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        fprintf(stderr, "screenshot: ioctl FBIOGET_VSCREENINFO failed\n");
        close(fb_fd);
        return 1;
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "screenshot: ioctl FBIOGET_FSCREENINFO failed\n");
        close(fb_fd);
        return 1;
    }

    size_t smem_len = finfo.smem_len;
    if (smem_len == 0) {
        smem_len = vinfo.yres * finfo.line_length;
    }

    void *fb_ptr = mmap(NULL, smem_len, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_ptr == MAP_FAILED || !fb_ptr) {
        fprintf(stderr, "screenshot: failed to mmap framebuffer\n");
        close(fb_fd);
        return 1;
    }

    /* Wrap framebuffer in a gfx_surface_t for BMP saving */
    gfx_surface_t s;
    memset(&s, 0, sizeof(s));
    s.width = (int) vinfo.xres;
    s.height = (int) vinfo.yres;
    s.pitch = (int) finfo.line_length;
    s.bpp = (int) (vinfo.bits_per_pixel / 8);
    s.pixels = (uint32_t *) fb_ptr;
    s.owns_pixels = 0;
    gfx_reset_clip(&s);

    int res = gfx_save_bmp(&s, out_path);
    munmap(fb_ptr, smem_len);
    close(fb_fd);

    if (res != 0) {
        fprintf(stderr, "screenshot: failed to save BMP to %s\n", out_path);
        return 1;
    }

    printf("[screenshot] Saved %lu x %lu screenshot to %s\n",
           (unsigned long)vinfo.xres, (unsigned long)vinfo.yres, out_path);
    return 0;
}

#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

struct fb_info {
    uint32_t phys_addr;
    void*    virt_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  type;
    uint8_t  red_pos;
    uint8_t  red_mask;
    uint8_t  green_pos;
    uint8_t  green_mask;
    uint8_t  blue_pos;
    uint8_t  blue_mask;
    int      present;
    int      is_wc;
};

extern struct fb_info g_fb_info;

void fb_init(uint32_t mb_info_addr);
int  fb_is_present(void);

#endif

#include "fb.h"
#include "multiboot2.h"
#include "ioremap.h"
#include "serial.h"
#include "vga.h"
#include "console.h"

struct fb_info g_fb_info;

int fb_is_present(void) {
    return g_fb_info.present;
}

void fb_init(uint32_t mb_info_addr) {
    g_fb_info.present = 0;
    if (!mb_info_addr) {
        serial_printf("[fb] No multiboot info; fallback to VGA text\n");
        return;
    }

    uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;
    uint32_t total_size = *(uint32_t*) ptr;
    uint8_t* end = ptr + total_size;
    uint8_t* tag_ptr = ptr + 8;

    struct mb2_tag_framebuffer* fb = NULL;

    while (tag_ptr < end) {
        struct mb2_tag* tag = (struct mb2_tag*) tag_ptr;
        if (tag->type == MB2_TAG_TYPE_END) break;

        if (tag->type == MB2_TAG_TYPE_FRAMEBUFFER) {
            fb = (struct mb2_tag_framebuffer*) tag;
            break;
        }

        tag_ptr += (tag->size + 7) & ~7u;
    }

    if (!fb) {
        serial_printf("[fb] No framebuffer tag found; fallback to VGA text\n");
        return;
    }

    /* Check if direct-RGB 32bpp */
    if (fb->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB || fb->bpp != 32) {
        serial_printf("[fb] Framebuffer unsupported type=%u bpp=%u; fallback to VGA text\n",
                      fb->framebuffer_type, fb->bpp);
        kprintf("[fb] Framebuffer unsupported type=%u bpp=%u; fallback to VGA text\n",
                fb->framebuffer_type, fb->bpp);
        return;
    }

    g_fb_info.phys_addr = (uint32_t) fb->addr;
    g_fb_info.width = fb->width;
    g_fb_info.height = fb->height;
    g_fb_info.pitch = fb->pitch;
    g_fb_info.bpp = fb->bpp;
    g_fb_info.type = fb->framebuffer_type;
    g_fb_info.red_pos = fb->red_field_position;
    g_fb_info.red_mask = fb->red_mask_size;
    g_fb_info.green_pos = fb->green_field_position;
    g_fb_info.green_mask = fb->green_mask_size;
    g_fb_info.blue_pos = fb->blue_field_position;
    g_fb_info.blue_mask = fb->blue_mask_size;

    uint32_t flags = 0;
    if (cpu_has_pat()) {
        flags = IOMAP_WRITECOMBINING;
        g_fb_info.is_wc = 1;
        serial_printf("[fb] Caching mode: PAT write-combining\n");
        kprintf("[fb] Caching mode: PAT write-combining\n");
    } else {
        flags = IOMAP_NOCACHE;
        g_fb_info.is_wc = 0;
        serial_printf("[fb] Caching mode: uncached (PCD)\n");
        kprintf("[fb] Caching mode: uncached (PCD)\n");
    }

    size_t fb_size = (size_t)g_fb_info.pitch * g_fb_info.height;
    g_fb_info.virt_addr = ioremap(g_fb_info.phys_addr, fb_size, flags);

    if (!g_fb_info.virt_addr) {
        serial_printf("[fb] Failed to ioremap framebuffer at 0x%08x; fallback to VGA text\n", g_fb_info.phys_addr);
        kprintf("[fb] Failed to ioremap framebuffer; fallback to VGA text\n");
        return;
    }

    g_fb_info.present = 1;
    console_switch_to_fb();

    serial_printf("[fb] %ux%ux%u pitch=%u at phys=0x%08x mapped at 0x%08x\n",
                  g_fb_info.width, g_fb_info.height, g_fb_info.bpp, g_fb_info.pitch,
                  g_fb_info.phys_addr, (uint32_t)g_fb_info.virt_addr);
    kprintf("[fb] %ux%ux%u pitch=%u at phys=0x%08x mapped at 0x%08x\n",
            g_fb_info.width, g_fb_info.height, g_fb_info.bpp, g_fb_info.pitch,
            g_fb_info.phys_addr, (uint32_t)g_fb_info.virt_addr);
}

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86

struct psf2_header {
    uint8_t  magic[4];       /* 0x72, 0xb5, 0x4a, 0x86 */
    uint32_t version;        /* zero */
    uint32_t headersize;     /* offset of bitmaps in file, 32 */
    uint32_t flags;          /* 0 if no unicode table */
    uint32_t length;         /* number of glyphs, 512 */
    uint32_t charsize;       /* number of bytes for each glyph, 16 */
    uint32_t height;         /* max height in pixels, 16 */
    uint32_t width;          /* max width in pixels, 8 */
} __attribute__((packed));

extern const uint8_t g_font_terminus16[8224];
extern const size_t g_font_terminus16_size;

const struct psf2_header* font_get_header(void);
const uint8_t* font_get_glyph(unsigned char c);

#endif

#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"

#pragma pack(push, 1)
struct bmp_file_header {
    uint16_t bf_type;       /* 'BM' = 0x4D42 */
    uint32_t bf_size;       /* file size in bytes */
    uint16_t bf_reserved1;
    uint16_t bf_reserved2;
    uint32_t bf_off_bits;   /* offset to pixel array (54) */
};

struct bmp_info_header {
    uint32_t bi_size;          /* 40 */
    int32_t  bi_width;
    int32_t  bi_height;        /* positive: bottom-up */
    uint16_t bi_planes;        /* 1 */
    uint16_t bi_bit_count;     /* 24 */
    uint32_t bi_compression;   /* 0 = BI_RGB */
    uint32_t bi_size_image;
    int32_t  bi_x_pels_per_meter;
    int32_t  bi_y_pels_per_meter;
    uint32_t bi_clr_used;
    uint32_t bi_clr_important;
};
#pragma pack(pop)

int gfx_save_bmp(const gfx_surface_t *s, const char *path) {
    if (!s || !s->pixels || !path || s->width <= 0 || s->height <= 0) {
        return -1;
    }

    int w = s->width;
    int h = s->height;
    int row_bytes = w * 3;
    int row_padded = (row_bytes + 3) & ~3;
    uint32_t image_size = (uint32_t)(row_padded * h);
    uint32_t file_size = 54 + image_size;

    uint8_t *file_buf = (uint8_t *) malloc((size_t) file_size);
    if (!file_buf) return -1;

    struct bmp_file_header *bfh = (struct bmp_file_header *) file_buf;
    bfh->bf_type = 0x4D42;
    bfh->bf_size = file_size;
    bfh->bf_reserved1 = 0;
    bfh->bf_reserved2 = 0;
    bfh->bf_off_bits = 54;

    struct bmp_info_header *bih = (struct bmp_info_header *) (file_buf + 14);
    bih->bi_size = sizeof(struct bmp_info_header);
    bih->bi_width = w;
    bih->bi_height = h; /* bottom-up */
    bih->bi_planes = 1;
    bih->bi_bit_count = 24;
    bih->bi_compression = 0;
    bih->bi_size_image = image_size;
    bih->bi_x_pels_per_meter = 2835; /* 72 DPI */
    bih->bi_y_pels_per_meter = 2835;
    bih->bi_clr_used = 0;
    bih->bi_clr_important = 0;

    int pitch_words = s->pitch / sizeof(uint32_t);
    uint8_t *dst_rows = file_buf + 54;

    /* Write rows from bottom (h - 1) to top (0) */
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *src_row = s->pixels + y * pitch_words;
        int row_idx = (h - 1 - y) * row_padded;
        uint8_t *dst = dst_rows + row_idx;
        int idx = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = src_row[x];
            dst[idx++] = (uint8_t) GFX_GET_B(c);
            dst[idx++] = (uint8_t) GFX_GET_G(c);
            dst[idx++] = (uint8_t) GFX_GET_R(c);
        }
        while (idx < row_padded) {
            dst[idx++] = 0;
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(file_buf);
        return -1;
    }

    size_t written = fwrite(file_buf, 1, file_size, f);
    free(file_buf);
    fclose(f);

    return (written == file_size) ? 0 : -1;
}

gfx_surface_t *gfx_load_image(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        return NULL;
    }

    unsigned char *file_data = (unsigned char *) malloc((size_t) fsize);
    if (!file_data) {
        fclose(f);
        return NULL;
    }

    if (fread(file_data, 1, (size_t) fsize, f) != (size_t) fsize) {
        free(file_data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    int w = 0, h = 0, channels = 0;
    unsigned char *raw_pixels = stbi_load_from_memory(file_data, (int) fsize, &w, &h, &channels, 4);
    free(file_data);

    if (!raw_pixels || w <= 0 || h <= 0) {
        return NULL;
    }

    gfx_surface_t *s = gfx_create_surface(w, h);
    if (!s) {
        stbi_image_free(raw_pixels);
        return NULL;
    }

    int total_pixels = w * h;
    for (int i = 0; i < total_pixels; i++) {
        unsigned char r = raw_pixels[i * 4 + 0];
        unsigned char g = raw_pixels[i * 4 + 1];
        unsigned char b = raw_pixels[i * 4 + 2];
        unsigned char a = raw_pixels[i * 4 + 3];
        s->pixels[i] = GFX_RGBA(r, g, b, a);
    }

    stbi_image_free(raw_pixels);
    return s;
}

#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Colors (32-bit ARGB/XRGB) */
#define GFX_RGB(r, g, b)        (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | ((uint32_t)(b)))
#define GFX_RGBA(r, g, b, a)    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | ((uint32_t)(b)))
#define GFX_GET_A(c)            (((c) >> 24) & 0xFF)
#define GFX_GET_R(c)            (((c) >> 16) & 0xFF)
#define GFX_GET_G(c)            (((c) >> 8) & 0xFF)
#define GFX_GET_B(c)            ((c) & 0xFF)

#define GFX_COLOR_BLACK         GFX_RGB(0, 0, 0)
#define GFX_COLOR_WHITE         GFX_RGB(255, 255, 255)
#define GFX_COLOR_RED           GFX_RGB(255, 0, 0)
#define GFX_COLOR_GREEN         GFX_RGB(0, 255, 0)
#define GFX_COLOR_BLUE          GFX_RGB(0, 0, 255)
#define GFX_COLOR_YELLOW        GFX_RGB(255, 255, 0)
#define GFX_COLOR_CYAN          GFX_RGB(0, 255, 255)
#define GFX_COLOR_MAGENTA       GFX_RGB(255, 0, 255)
#define GFX_COLOR_GRAY          GFX_RGB(128, 128, 128)
#define GFX_COLOR_LIGHT_GRAY    GFX_RGB(200, 200, 200)
#define GFX_COLOR_DARK_GRAY     GFX_RGB(50, 50, 50)

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
} gfx_rect_t;

typedef struct {
    int width;
    int height;
    int pitch;      /* bytes per row */
    int bpp;        /* bytes per pixel, 4 for 32-bit */
    uint32_t *pixels;
    gfx_rect_t clip;
    int owns_pixels;
} gfx_surface_t;

typedef struct gfx_font gfx_font_t;

/* Graphics context (screen framebuffer + backbuffer + input) */
typedef struct {
    int fb_fd;
    int mouse_fd;
    int kbd_fd;
    int width;
    int height;
    int pitch;
    int bpp;
    uint32_t *fb_mmap;
    size_t fb_size;
    gfx_surface_t *backbuffer;
    int mouse_x;
    int mouse_y;
    int mouse_buttons;
    int cursor_visible;
} gfx_context_t;

/* Event subsystem */
typedef enum {
    GFX_EVENT_NONE = 0,
    GFX_EVENT_KEY_DOWN,
    GFX_EVENT_KEY_UP,
    GFX_EVENT_MOUSE_MOVE,
    GFX_EVENT_MOUSE_DOWN,
    GFX_EVENT_MOUSE_UP,
    GFX_EVENT_MOUSE_WHEEL
} gfx_event_type_t;

typedef struct {
    gfx_event_type_t type;
    union {
        struct {
            int keycode;
            int modifiers;
        } key;
        struct {
            int x, y;
            int dx, dy;
            int button;   /* 1=left, 2=right, 3=middle */
            int buttons;  /* bitmask */
            int wheel;    /* +1 or -1 */
        } mouse;
    };
} gfx_event_t;

/* Context Management */
gfx_context_t *gfx_open(void);
void gfx_close(gfx_context_t *ctx);
void gfx_flip(gfx_context_t *ctx);
void gfx_flip_rect(gfx_context_t *ctx, int x, int y, int w, int h);

/* Surface Management */
gfx_surface_t *gfx_create_surface(int width, int height);
void gfx_destroy_surface(gfx_surface_t *s);
void gfx_set_clip(gfx_surface_t *s, int x0, int y0, int x1, int y1);
void gfx_reset_clip(gfx_surface_t *s);

/* 2D Drawing Primitives */
void gfx_clear(gfx_surface_t *s, uint32_t color);
void gfx_putpixel(gfx_surface_t *s, int x, int y, uint32_t color);
uint32_t gfx_getpixel(const gfx_surface_t *s, int x, int y);
uint32_t gfx_blend(uint32_t dst, uint32_t src);
void gfx_putpixel_alpha(gfx_surface_t *s, int x, int y, uint32_t color);

void gfx_draw_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_draw_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect_alpha(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_draw_circle(gfx_surface_t *s, int xc, int yc, int r, uint32_t color);
void gfx_fill_circle(gfx_surface_t *s, int xc, int yc, int r, uint32_t color);

/* Blitting */
void gfx_blit(gfx_surface_t *dst, int dx, int dy, const gfx_surface_t *src, int sx, int sy, int sw, int sh);
void gfx_blit_alpha(gfx_surface_t *dst, int dx, int dy, const gfx_surface_t *src, int sx, int sy, int sw, int sh);

/* Bitmap Text Rendering (embedded 8x16 font) */
void gfx_draw_char(gfx_surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void gfx_draw_text_transparent(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg);

/* TrueType Font Rendering */
gfx_font_t *gfx_load_font(const char *path, float pixel_height);
void gfx_destroy_font(gfx_font_t *font);
void gfx_draw_text_ttf(gfx_surface_t *s, gfx_font_t *font, int x, int y, const char *str, uint32_t color);
int gfx_measure_text_ttf(gfx_font_t *font, const char *str, int *out_w, int *out_h);

/* Image I/O */
gfx_surface_t *gfx_load_image(const char *path);
int gfx_save_bmp(const gfx_surface_t *s, const char *path);

/* Event & Cursor */
int gfx_poll_event(gfx_context_t *ctx, gfx_event_t *event);
int gfx_wait_event(gfx_context_t *ctx, gfx_event_t *event, int timeout_ms);
void gfx_draw_cursor(gfx_surface_t *s, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */

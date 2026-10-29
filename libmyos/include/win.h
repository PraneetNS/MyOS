#ifndef _WIN_H
#define _WIN_H

#include <stdint.h>
#include <gfx.h>
#include <win_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct win_conn win_conn_t;
typedef struct win win_t;

/* Event Types */
enum {
    WIN_EVENT_NONE = 0,
    WIN_EVENT_KEY,
    WIN_EVENT_POINTER_ENTER,
    WIN_EVENT_POINTER_LEAVE,
    WIN_EVENT_POINTER_MOVE,
    WIN_EVENT_POINTER_BUTTON,
    WIN_EVENT_POINTER_WHEEL,
    WIN_EVENT_FOCUS,
    WIN_EVENT_CLOSE_REQUEST,
    WIN_EVENT_CONFIGURE,
    WIN_EVENT_FRAME_DONE
};

/* Unified Event Structure */
typedef struct win_event {
    uint32_t type;       /* WIN_EVENT_* */
    win_t*   window;     /* Window targeted by event */
    union {
        struct {
            uint32_t keycode;
            uint32_t unicode;
            uint32_t modifiers;
            uint32_t action;  /* WIN_KEY_PRESS, WIN_KEY_RELEASE, etc. */
        } key;
        struct {
            int32_t  x;
            int32_t  y;
            int32_t  dx;
            int32_t  dy;
            uint32_t button;  /* 1=left, 2=right, 3=middle */
            uint32_t state;   /* 1=pressed, 0=released */
            uint32_t buttons; /* Bitmask of pressed buttons */
            int32_t  wheel;   /* Wheel delta */
        } pointer;
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t state;   /* WIN_STATE_* */
        } configure;
        struct {
            uint32_t focused; /* 1=in, 0=out */
        } focus;
    };
} win_event_t;

/* Connection Lifecycle */
win_conn_t* win_connect(void);
void        win_disconnect(win_conn_t* conn);
int         win_fd(win_conn_t* conn);
uint32_t    win_screen_width(win_conn_t* conn);
uint32_t    win_screen_height(win_conn_t* conn);

/* Window Management */
win_t*      win_create(win_conn_t* conn, int width, int height, uint32_t flags, const char* title);
void        win_destroy(win_t* win);
uint32_t    win_id(win_t* win);
void        win_set_title(win_t* win, const char* title);
void        win_set_min_max_size(win_t* win, int min_w, int min_h, int max_w, int max_h);
void        win_set_state(win_t* win, uint32_t state);
void        win_request_move(win_t* win);
int         win_width(win_t* win);
int         win_height(win_t* win);

/* Rendering & Buffers */
gfx_surface_t* win_buffer(win_t* win);
void           win_damage(win_t* win, int x, int y, int w, int h);
void           win_commit(win_t* win);

/* Event Dispatching */
int win_event_wait(win_conn_t* conn, win_event_t* event, int timeout_ms);
int win_event_poll(win_conn_t* conn, win_event_t* event);

/* Clipboard Integration */
void win_clipboard_set(win_conn_t* conn, const char* mime, const void* data, uint32_t len);
int  win_clipboard_get(win_conn_t* conn, const char* mime, void* buf, uint32_t max_len);

/* Accessibility Integration */
typedef void (*win_a11y_tree_handler_t)(win_t* win, uint32_t root_id, uint32_t max_depth, void* udata);
typedef void (*win_a11y_action_handler_t)(win_t* win, uint32_t node_id, const char* action, const char* args, void* udata);

void win_set_a11y_handlers(win_t* win, win_a11y_tree_handler_t tree_fn, win_a11y_action_handler_t act_fn, void* udata);
void win_send_a11y_tree(win_t* win, const char* json_tree, int truncated);
void win_send_a11y_result(win_t* win, int ok, const char* error, const char* new_state);

#ifdef __cplusplus
}
#endif

#endif /* _WIN_H */

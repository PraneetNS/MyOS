#ifndef _WIN_PROTOCOL_H
#define _WIN_PROTOCOL_H

#include <stdint.h>

#define WIN_PROTO_VERSION    1
#define WIN_SOCK_PATH        "/run/display.sock"
#define WIN_MAX_MSG_LEN      4096

/* Message Framing Header (12 bytes) */
struct win_msg_header {
    uint32_t len;        /* Total message size including header */
    uint16_t type;       /* WIN_MSG_* */
    uint16_t flags;      /* Reserved / flags */
    uint32_t window_id;  /* Target window ID (0 for connection-level) */
};

/* Message Types */
enum {
    /* Client -> Server */
    WIN_MSG_HELLO            = 1,
    WIN_MSG_CREATE_WINDOW    = 2,
    WIN_MSG_DESTROY_WINDOW   = 3,
    WIN_MSG_SET_TITLE        = 4,
    WIN_MSG_SET_MIN_MAX_SIZE = 5,
    WIN_MSG_ATTACH_BUFFER    = 6,
    WIN_MSG_DAMAGE           = 7,
    WIN_MSG_COMMIT           = 8,
    WIN_MSG_CONFIGURE_ACK    = 9,
    WIN_MSG_CLIPBOARD_SET    = 10,
    WIN_MSG_CLIPBOARD_GET    = 11,
    WIN_MSG_REQUEST_MOVE     = 12,
    WIN_MSG_SET_STATE        = 13,

    /* Control Client -> Server */
    WIN_MSG_CTRL_HELLO       = 50,
    WIN_MSG_CTRL_LIST        = 51,
    WIN_MSG_CTRL_ACTION      = 52,
    WIN_MSG_CTRL_STATS       = 53,
    WIN_MSG_CTRL_SCREENSHOT  = 54,

    /* Server -> Client */
    WIN_MSG_HELLO_ACK        = 100,
    WIN_MSG_CONFIGURE        = 101,
    WIN_MSG_KEY              = 102,
    WIN_MSG_POINTER_ENTER    = 103,
    WIN_MSG_POINTER_LEAVE    = 104,
    WIN_MSG_POINTER_MOVE     = 105,
    WIN_MSG_POINTER_BUTTON   = 106,
    WIN_MSG_POINTER_WHEEL    = 107,
    WIN_MSG_FOCUS            = 108,
    WIN_MSG_CLOSE_REQUEST    = 109,
    WIN_MSG_BUFFER_RELEASE   = 110,
    WIN_MSG_FRAME_DONE       = 111,
    WIN_MSG_CLIPBOARD_DATA   = 112,

    /* Server -> Control Client */
    WIN_MSG_CTRL_ACK         = 200,
    WIN_MSG_CTRL_LIST_ENTRY  = 201,
    WIN_MSG_CTRL_STATS_DATA  = 202
};

/* Buffer Pixel Formats */
#define WIN_FORMAT_XRGB8888  1
#define WIN_FORMAT_ARGB8888  2

/* Window States */
#define WIN_STATE_NORMAL     0
#define WIN_STATE_MAXIMIZED  1
#define WIN_STATE_MINIMIZED  2
#define WIN_STATE_FULLSCREEN 3

/* Window Flags */
#define WIN_FLAG_BORDERLESS  (1 << 0)
#define WIN_FLAG_RESIZABLE   (1 << 1)
#define WIN_FLAG_POPUP       (1 << 2)
#define WIN_FLAG_ALWAYS_TOP  (1 << 3)

/* Key Actions */
#define WIN_KEY_PRESS        1
#define WIN_KEY_RELEASE      2
#define WIN_KEY_REPEAT       3

/* Payloads */
struct win_msg_hello {
    uint32_t version;
    uint32_t pid;
};

struct win_msg_hello_ack {
    uint32_t screen_w;
    uint32_t screen_h;
};

struct win_msg_create_window {
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    char title[128];
};

struct win_msg_set_title {
    char title[128];
};

struct win_msg_set_min_max_size {
    uint32_t min_w;
    uint32_t min_h;
    uint32_t max_w;
    uint32_t max_h;
};

struct win_msg_attach_buffer {
    uint32_t buffer_index;
    char shm_name[64];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};

struct win_msg_damage {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct win_msg_commit {
    uint32_t buffer_index;
};

struct win_msg_set_state {
    uint32_t state;
};

struct win_msg_configure {
    uint32_t width;
    uint32_t height;
    uint32_t state;
};

struct win_msg_key {
    uint32_t keycode;
    uint32_t unicode;
    uint32_t modifiers;
    uint32_t action;
};

struct win_msg_pointer_enter {
    int32_t x;
    int32_t y;
};

struct win_msg_pointer_move {
    int32_t x;
    int32_t y;
};

struct win_msg_pointer_button {
    uint32_t button;
    uint32_t state;
    int32_t x;
    int32_t y;
};

struct win_msg_pointer_wheel {
    int32_t dx;
    int32_t dy;
};

struct win_msg_focus {
    uint32_t focused;
};

struct win_msg_buffer_release {
    uint32_t buffer_index;
};

struct win_msg_clipboard_set {
    char mime[32];
    uint32_t len;
    char data[1024];
};

struct win_msg_clipboard_get {
    char mime[32];
};

struct win_msg_clipboard_data {
    char mime[32];
    uint32_t len;
    char data[1024];
};

/* Control Action Definitions */
#define WIN_CTRL_ACT_FOCUS     1
#define WIN_CTRL_ACT_CLOSE     2
#define WIN_CTRL_ACT_MOVE      3
#define WIN_CTRL_ACT_RESIZE    4
#define WIN_CTRL_ACT_MAXIMIZE  5
#define WIN_CTRL_ACT_MINIMIZE  6
#define WIN_CTRL_ACT_RESTORE   7
#define WIN_CTRL_ACT_QUIT      8

struct win_msg_ctrl_action {
    uint32_t target_window_id;
    uint32_t action;
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
};

struct win_msg_ctrl_screenshot {
    char path[128];
};

struct win_msg_ctrl_list_entry {
    uint32_t window_id;
    uint32_t pid;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t state;
    uint32_t focused;
    char title[128];
};

struct win_msg_ctrl_stats_data {
    uint32_t composite_count;
    uint32_t avg_composite_us;
    uint32_t max_composite_us;
    uint32_t bytes_flushed_kb;
    uint32_t active_clients;
    uint32_t active_windows;
};

#endif /* _WIN_PROTOCOL_H */

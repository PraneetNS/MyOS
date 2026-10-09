#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* Linux-compatible input event representation */
struct input_event {
    uint32_t tick_ms;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_SYN      0
#define EV_KEY      1
#define EV_REL      2
#define EV_ABS      3
#define EV_MSC      4

#define SYN_REPORT  0

/* REL codes */
#define REL_X       0
#define REL_Y       1
#define REL_WHEEL   8

/* Mouse button keycodes */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* ImPS/2 packet size exposed on /dev/input/mice */
#define IMPS2_PACKET_SIZE  3

typedef struct {
    uint8_t buttons;
    int8_t  dx;
    int8_t  dy;
} imps2_packet_t;

/* Initialise the input subsystem */
void input_init(void);

/* Producer interfaces */
void input_push_mouse_events(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void input_push_mouse_raw(uint8_t buttons, int8_t dx, int8_t dy);
void input_push_key(uint16_t keycode, int32_t value);

/* Consumer interfaces for devfs */
int input_read_mouse0(uint8_t *buf, uint32_t count, int nonblock);
int input_read_kbd0(uint8_t *buf, uint32_t count, int nonblock);
int input_read_mice(uint8_t *buf, uint32_t count, int nonblock);

/* Polling checks */
int input_mouse0_poll(void);
int input_kbd0_poll(void);
int input_mice_poll(void);

/* Wait channel tokens */
extern int g_mouse0_channel_token;
extern int g_kbd0_channel_token;
extern int g_mice_channel_token;

#endif /* INPUT_H */

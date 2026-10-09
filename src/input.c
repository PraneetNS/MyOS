/*
 * input.c - Generic input event layer (Stage 18A Step 4)
 *
 * Implements:
 *   - /dev/input/mouse0 ring (struct input_event, 256 events)
 *   - /dev/input/kbd0 ring (struct input_event, 256 events)
 *   - /dev/input/mice ring (ImPS/2 3-byte packets, 256 packets)
 */
#include "input.h"
#include "timer.h"
#include "scheduler.h"
#include "serial.h"
#include "errno.h"
#include "signal.h"
#include <string.h>

#define RING_CAP 256  /* Must be a power of 2 */
#define RING_MASK (RING_CAP - 1)

/* Wait channel tokens */
int g_mouse0_channel_token = 0;
int g_kbd0_channel_token   = 0;
int g_mice_channel_token   = 0;

/* --- /dev/input/mouse0 ring --- */
static struct input_event mouse0_ring[RING_CAP];
static volatile uint32_t mouse0_head = 0;
static volatile uint32_t mouse0_tail = 0;
static uint8_t prev_mouse_buttons = 0;

/* --- /dev/input/kbd0 ring --- */
static struct input_event kbd0_ring[RING_CAP];
static volatile uint32_t kbd0_head = 0;
static volatile uint32_t kbd0_tail = 0;

/* --- /dev/input/mice ring --- */
static imps2_packet_t mice_ring[RING_CAP];
static volatile uint32_t mice_head = 0;
static volatile uint32_t mice_tail = 0;

void input_init(void) {
    mouse0_head = mouse0_tail = 0;
    kbd0_head   = kbd0_tail   = 0;
    mice_head   = mice_tail   = 0;
    prev_mouse_buttons = 0;
    serial_printf("[input] input event layer ready\n");
}

/* Helper to enqueue a struct input_event into mouse0 */
static void enqueue_mouse0(uint32_t tick, uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (mouse0_head + 1) & RING_MASK;
    if (next == mouse0_tail) {
        /* Drop oldest on overflow */
        mouse0_tail = (mouse0_tail + 1) & RING_MASK;
    }
    mouse0_ring[mouse0_head].tick_ms = tick;
    mouse0_ring[mouse0_head].type    = type;
    mouse0_ring[mouse0_head].code    = code;
    mouse0_ring[mouse0_head].value   = value;
    mouse0_head = next;
}

/* Helper to enqueue a struct input_event into kbd0 */
static void enqueue_kbd0(uint32_t tick, uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (kbd0_head + 1) & RING_MASK;
    if (next == kbd0_tail) {
        /* Drop oldest on overflow */
        kbd0_tail = (kbd0_tail + 1) & RING_MASK;
    }
    kbd0_ring[kbd0_head].tick_ms = tick;
    kbd0_ring[kbd0_head].type    = type;
    kbd0_ring[kbd0_head].code    = code;
    kbd0_ring[kbd0_head].value   = value;
    kbd0_head = next;
}

void input_push_mouse_events(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    uint32_t tick = timer_get_ticks() * 10;
    int pushed = 0;

    if (dx != 0) {
        enqueue_mouse0(tick, EV_REL, REL_X, dx);
        pushed = 1;
    }
    if (dy != 0) {
        enqueue_mouse0(tick, EV_REL, REL_Y, dy);
        pushed = 1;
    }
    if (wheel != 0) {
        enqueue_mouse0(tick, EV_REL, REL_WHEEL, wheel);
        pushed = 1;
    }

    /* Button transitions */
    if ((buttons & 0x01) != (prev_mouse_buttons & 0x01)) {
        enqueue_mouse0(tick, EV_KEY, BTN_LEFT, (buttons & 0x01) ? 1 : 0);
        pushed = 1;
    }
    if ((buttons & 0x02) != (prev_mouse_buttons & 0x02)) {
        enqueue_mouse0(tick, EV_KEY, BTN_RIGHT, (buttons & 0x02) ? 1 : 0);
        pushed = 1;
    }
    if ((buttons & 0x04) != (prev_mouse_buttons & 0x04)) {
        enqueue_mouse0(tick, EV_KEY, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);
        pushed = 1;
    }
    prev_mouse_buttons = buttons;

    if (pushed) {
        enqueue_mouse0(tick, EV_SYN, SYN_REPORT, 0);
        scheduler_wake_channel(&g_mouse0_channel_token);
    }
}

void input_push_mouse_raw(uint8_t buttons, int8_t dx, int8_t dy) {
    uint32_t next = (mice_head + 1) & RING_MASK;
    if (next == mice_tail) {
        mice_tail = (mice_tail + 1) & RING_MASK;
    }
    mice_ring[mice_head].buttons = buttons;
    mice_ring[mice_head].dx      = dx;
    mice_ring[mice_head].dy      = dy;
    mice_head = next;

    scheduler_wake_channel(&g_mice_channel_token);
}

void input_push_key(uint16_t keycode, int32_t value) {
    uint32_t tick = timer_get_ticks() * 10;
    enqueue_kbd0(tick, EV_KEY, keycode, value);
    enqueue_kbd0(tick, EV_SYN, SYN_REPORT, 0);
    scheduler_wake_channel(&g_kbd0_channel_token);
}

/* --- Read functions --- */

int input_read_mouse0(uint8_t *buf, uint32_t count, int nonblock) {
    if (count == 0) return 0;
    uint32_t ev_size = sizeof(struct input_event);
    count = (count / ev_size) * ev_size;
    if (count == 0) return 0;

    while (mouse0_head == mouse0_tail) {
        if (nonblock) {
            return -EAGAIN;
        }
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
        scheduler_wait_channel(&g_mouse0_channel_token);
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
    }

    uint32_t n = 0;
    while (n + ev_size <= count && mouse0_head != mouse0_tail) {
        struct input_event *ev = (struct input_event*) (buf + n);
        *ev = mouse0_ring[mouse0_tail];
        mouse0_tail = (mouse0_tail + 1) & RING_MASK;
        n += ev_size;
    }
    return (int) n;
}

int input_read_kbd0(uint8_t *buf, uint32_t count, int nonblock) {
    if (count == 0) return 0;
    uint32_t ev_size = sizeof(struct input_event);
    count = (count / ev_size) * ev_size;
    if (count == 0) return 0;

    while (kbd0_head == kbd0_tail) {
        if (nonblock) {
            return -EAGAIN;
        }
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
        scheduler_wait_channel(&g_kbd0_channel_token);
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
    }

    uint32_t n = 0;
    while (n + ev_size <= count && kbd0_head != kbd0_tail) {
        struct input_event *ev = (struct input_event*) (buf + n);
        *ev = kbd0_ring[kbd0_tail];
        kbd0_tail = (kbd0_tail + 1) & RING_MASK;
        n += ev_size;
    }
    return (int) n;
}

int input_read_mice(uint8_t *buf, uint32_t count, int nonblock) {
    if (count == 0) return 0;
    count = (count / IMPS2_PACKET_SIZE) * IMPS2_PACKET_SIZE;
    if (count == 0) return 0;

    while (mice_head == mice_tail) {
        if (nonblock) {
            return -EAGAIN;
        }
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
        scheduler_wait_channel(&g_mice_channel_token);
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
    }

    uint32_t n = 0;
    while (n + IMPS2_PACKET_SIZE <= count && mice_head != mice_tail) {
        imps2_packet_t *pkt = &mice_ring[mice_tail];
        mice_tail = (mice_tail + 1) & RING_MASK;

        buf[n + 0] = pkt->buttons;
        buf[n + 1] = (uint8_t) pkt->dx;
        buf[n + 2] = (uint8_t) pkt->dy;
        n += IMPS2_PACKET_SIZE;
    }
    return (int) n;
}

/* Polling helpers */
int input_mouse0_poll(void) {
    return mouse0_head != mouse0_tail;
}

int input_kbd0_poll(void) {
    return kbd0_head != kbd0_tail;
}

int input_mice_poll(void) {
    return mice_head != mice_tail;
}

/*
 * mouse.c - PS/2 mouse driver with IntelliMouse extension (Stage 18A Step 4)
 *
 * Protocol:
 *   i8042 port 0x60 = data, 0x64 = command/status
 *
 *   Enabling the auxiliary device:
 *     1. Send 0xA8 to 0x64  -- enable aux
 *     2. Read CCP (0x20), set bit 1 (IRQ12 enable), clear bit 5 (disable aux clock)
 *     3. Send 0x60 + new CCP to re-configure controller
 *     4. Reset mouse (0xFF)
 *     5. Set defaults (0xF6)
 *     6. IntelliMouse wheel sequence: sample rate 200, 100, 80; read ID
 *     7. Enable streaming (0xF4)
 *
 *   Each IRQ12 checks status bit 5 (AUXB) to guarantee data is from mouse.
 *   Accumulates 3-byte (or 4-byte if wheel supported) packets:
 *     byte 0: buttons + overflow/sign bits
 *     byte 1: dx (signed)
 *     byte 2: dy (signed, PS/2 "up positive")
 *     byte 3: wheel (signed 4-bit) if IntelliMouse mode enabled
 *
 *   Pushes events to input subsystem:
 *     input_push_mouse_events() (for /dev/input/mouse0)
 *     input_push_mouse_raw()    (for /dev/input/mice)
 */
#include "mouse.h"
#include "input.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include <string.h>

/* i8042 ports */
#define I8042_DATA 0x60
#define I8042_STAT 0x64
#define I8042_CMD  0x64

/* Status register bits */
#define I8042_STAT_OBF  0x01   /* output buffer full (data available) */
#define I8042_STAT_IBF  0x02   /* input buffer full (controller busy)  */
#define I8042_STAT_AUX  0x20   /* output buffer contains AUX data      */

/* Controller commands */
#define I8042_READ_CCP   0x20   /* read current command byte */
#define I8042_WRITE_CCP  0x60   /* write command byte */
#define I8042_AUX_ENABLE 0xA8   /* enable auxiliary device */
#define I8042_AUX_SEND   0xD4   /* next byte goes to aux device */

/* Mouse commands */
#define MOUSE_RESET        0xFF
#define MOUSE_ENABLE       0xF4  /* enable streaming */
#define MOUSE_DISABLE      0xF5
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_SET_SAMPLE   0xF3
#define MOUSE_GET_ID       0xF2
#define MOUSE_ACK          0xFA

/* Timeout loops for i8042 wait */
#define I8042_TIMEOUT 100000

static int mouse_ok = 0;          /* 1 = driver successfully initialised */
static int mouse_has_wheel = 0;   /* 1 = IntelliMouse 4-byte stream */
static int mouse_packet_len = 3;  /* 3 or 4 */
static int mouse_debug = 0;       /* 1 = dump packets to serial */

/* ------------------------------------------------------------------ */
/* Low-level i8042 helpers                                              */
/* ------------------------------------------------------------------ */

static void i8042_wait_write(void) {
    for (int i = 0; i < I8042_TIMEOUT; i++) {
        if (!(inb(I8042_STAT) & I8042_STAT_IBF)) return;
    }
}

static void i8042_wait_read(void) {
    for (int i = 0; i < I8042_TIMEOUT; i++) {
        if (inb(I8042_STAT) & I8042_STAT_OBF) return;
    }
}

static void i8042_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(I8042_STAT) & I8042_STAT_OBF)) break;
        (void) inb(I8042_DATA);
    }
}

static void aux_write(uint8_t val) {
    i8042_wait_write();
    outb(I8042_CMD, I8042_AUX_SEND);
    i8042_wait_write();
    outb(I8042_DATA, val);
}

static uint8_t aux_read(void) {
    i8042_wait_read();
    return inb(I8042_DATA);
}

/* ------------------------------------------------------------------ */
/* IRQ12 handler                                                        */
/* ------------------------------------------------------------------ */

static uint8_t mouse_buf[4];
static int     mouse_phase = 0;

static void mouse_irq_handler(struct registers *regs) {
    (void) regs;

    /* Verify this byte is actually from the auxiliary device */
    uint8_t status = inb(I8042_STAT);
    if (!(status & I8042_STAT_AUX)) {
        return;
    }

    uint8_t byte = inb(I8042_DATA);

    switch (mouse_phase) {
        case 0:
            /* First byte: bit 3 must be set per PS/2 spec.
               If not, we are desynced -- discard and stay at phase 0. */
            if (!(byte & 0x08)) {
                if (mouse_debug) {
                    serial_printf("[mouse] desync (byte0=0x%02x), resyncing\n", byte);
                }
                return;
            }
            mouse_buf[0] = byte;
            mouse_phase = 1;
            break;

        case 1:
            mouse_buf[1] = byte;
            mouse_phase = 2;
            break;

        case 2:
            mouse_buf[2] = byte;
            if (mouse_has_wheel) {
                mouse_phase = 3;
            } else {
                mouse_phase = 0;
                goto process_packet;
            }
            break;

        case 3:
            mouse_buf[3] = byte;
            mouse_phase = 0;
            goto process_packet;
    }
    return;

process_packet:
    {
        uint8_t flags = mouse_buf[0];
        int8_t  dx    = (int8_t) mouse_buf[1];
        int8_t  dy    = (int8_t) mouse_buf[2];
        int8_t  wheel = 0;

        if (mouse_has_wheel) {
            /* Lower 4 bits are signed wheel movement */
            int8_t w = (int8_t)(mouse_buf[3] & 0x0F);
            if (w & 0x08) w |= 0xF0; /* sign extend */
            wheel = w;
        }

        /* Sign extension from status bits */
        if (flags & 0x10) dx = (int8_t)(mouse_buf[1] | 0x100);
        if (flags & 0x20) dy = (int8_t)(mouse_buf[2] | 0x100);

        /* Discard on overflow */
        if ((flags & 0x40) || (flags & 0x80)) return;

        /* Buttons: bit0=left, bit1=right, bit2=middle */
        uint8_t buttons = flags & 0x07;

        /* Invert dy so positive is down in screen coordinates */
        dy = -dy;

        if (mouse_debug) {
            serial_printf("[mouse] pkt: btn=0x%x dx=%d dy=%d wheel=%d\n", buttons, dx, dy, wheel);
        }

        /* Push to both raw stream (/dev/input/mice) and event layer (/dev/input/mouse0) */
        input_push_mouse_raw(buttons, dx, dy);
        input_push_mouse_events(buttons, dx, dy, wheel);
    }
}

/* ------------------------------------------------------------------ */
/* Public init                                                          */
/* ------------------------------------------------------------------ */

void mouse_install(void) {
    serial_printf("[mouse] initialising PS/2 auxiliary device...\n");

    i8042_flush();

    /* 1. Enable auxiliary port */
    i8042_wait_write();
    outb(I8042_CMD, I8042_AUX_ENABLE);

    /* 2. Read command byte, enable IRQ12 (bit 1), enable aux clock (clear bit 5) */
    i8042_wait_write();
    outb(I8042_CMD, I8042_READ_CCP);
    i8042_wait_read();
    uint8_t ccp = inb(I8042_DATA);
    ccp |=  0x02;   /* enable IRQ12 */
    ccp &= ~0x20;   /* enable aux clock */

    /* 3. Write new CCP */
    i8042_wait_write();
    outb(I8042_CMD, I8042_WRITE_CCP);
    i8042_wait_write();
    outb(I8042_DATA, ccp);

    /* 4. Reset mouse */
    aux_write(MOUSE_RESET);
    (void) aux_read();  /* ACK (0xFA) */
    (void) aux_read();  /* Self-test passed (0xAA) */
    (void) aux_read();  /* Device ID (0x00) */

    /* 5. Set default settings (0xF6) */
    aux_write(MOUSE_SET_DEFAULTS);
    (void) aux_read();  /* ACK */

    /* 6. Try IntelliMouse wheel detection: sample rates 200, 100, 80 */
    aux_write(MOUSE_SET_SAMPLE); (void) aux_read();
    aux_write(200);              (void) aux_read();
    aux_write(MOUSE_SET_SAMPLE); (void) aux_read();
    aux_write(100);              (void) aux_read();
    aux_write(MOUSE_SET_SAMPLE); (void) aux_read();
    aux_write(80);               (void) aux_read();

    aux_write(MOUSE_GET_ID);
    (void) aux_read(); /* ACK */
    uint8_t dev_id = aux_read();

    if (dev_id == 3 || dev_id == 4) {
        mouse_has_wheel  = 1;
        mouse_packet_len = 4;
    } else {
        mouse_has_wheel  = 0;
        mouse_packet_len = 3;
    }

    /* 7. Enable streaming (0xF4) */
    aux_write(MOUSE_ENABLE);
    (void) aux_read(); /* ACK */

    /* 8. Install IRQ12 handler (vector 44 = 32+12) */
    register_interrupt_handler(44, mouse_irq_handler);

    mouse_ok    = 1;
    mouse_phase = 0;
    serial_printf("[mouse] PS/2 mouse ready (IRQ12, %d-byte stream%s)\n",
                  mouse_packet_len, mouse_has_wheel ? ", IntelliMouse wheel enabled" : "");
}

void mouse_set_debug(int enable) {
    mouse_debug = enable;
}

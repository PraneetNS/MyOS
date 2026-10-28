#include "serial.h"
#include "io.h"
#include "vga.h"
#include <stdarg.h>

#include "tty.h"
#include "idt.h"
#include "errno.h"
#include "scheduler.h"
#include "signal.h"
#include "vfs.h"

#define COM1 COM1_PORT

static void serial_callback(struct registers* regs) {
    (void) regs;
    tty_t* tty = tty_get_global();
    while (serial_received()) {
        char c = serial_read();
        if (tty) {
            tty_input_char(tty, c);
        }
    }
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* Disable all interrupts initially */
    outb(COM1 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x01);    /* Set divisor to 1 (115200 baud) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit (8N1) */
    outb(COM1 + 2, 0xC7);    /* Enable FIFO, clear them, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

void serial_install_irq(void) {
    register_interrupt_handler(36, &serial_callback); /* IRQ4 = vector 36 */
    outb(COM1 + 1, 0x01);    /* Enable Received Data Available interrupt */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, OUT2 bit set */
}

int serial_received(void) {
    return inb(COM1 + 5) & 1;
}

char serial_read(void) {
    while (serial_received() == 0);
    return inb(COM1);
}

static inline int serial_is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    if (c == '\n') {
        while (serial_is_transmit_empty() == 0);
        outb(COM1, '\r');
    }
    while (serial_is_transmit_empty() == 0);
    outb(COM1, c);
}

void serial_puts(const char* s) {
    if (!s) return;
    while (*s) {
        serial_putc(*s++);
    }
}

static void print_number(void (*put_func)(char), uint32_t n, int base, int is_signed, int width, char pad) {
    char buf[32];
    int i = 0;
    const char digits[] = "0123456789abcdef";
    int negative = 0;

    if (is_signed && (int32_t)n < 0) {
        negative = 1;
        n = -(int32_t)n;
    }

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = digits[n % base];
            n /= base;
        }
    }

    if (negative) {
        if (pad == '0') {
            put_func('-');
            negative = 0;
            if (width > 0) width--;
        } else {
            buf[i++] = '-';
        }
    }

    while (width > i) {
        put_func(pad);
        width--;
    }

    while (i > 0) {
        put_func(buf[--i]);
    }
}

static void kvprintf(void (*put_func)(char), const char* fmt, va_list args) {
    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            put_func(fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0') break;

        char pad = ' ';
        int width = 0;
        if (fmt[i] == '0') {
            pad = '0';
            i++;
        }
        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }
        if (fmt[i] == '\0') break;

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                put_func(c);
                break;
            }
            case 's': {
                const char* str = va_arg(args, const char*);
                if (!str) str = "(null)";
                while (*str) put_func(*str++);
                break;
            }
            case 'd':
            case 'i': {
                int val = va_arg(args, int);
                print_number(put_func, (uint32_t)val, 10, 1, width, pad);
                break;
            }
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                print_number(put_func, val, 10, 0, width, pad);
                break;
            }
            case 'x':
            case 'p': {
                unsigned int val = va_arg(args, unsigned int);
                print_number(put_func, val, 16, 0, width, pad);
                break;
            }
            case '%': {
                put_func('%');
                break;
            }
            default: {
                put_func('%');
                put_func(fmt[i]);
                break;
            }
        }
    }
}

void serial_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(serial_putc, fmt, args);
    va_end(args);
}

static void kprint_char(char c) {
    terminal_putchar(c);
    serial_putc(c);
}

void kprintf(const char* fmt, ...) {
    uint32_t saved_eflags;
    asm volatile ("pushf; pop %0; cli" : "=r"(saved_eflags));

    va_list args;
    va_start(args, fmt);
    kvprintf(kprint_char, fmt, args);
    va_end(args);

    if (saved_eflags & 0x200) {
        asm volatile ("sti");
    }
}

/* --- COM2 (/dev/serial1) raw byte device --- */
#define COM2 COM2_PORT
#define SERIAL1_RX_BUF_SIZE 8192

static uint8_t s_serial1_rx_buf[SERIAL1_RX_BUF_SIZE];
static volatile uint32_t s_serial1_rx_head = 0;
static volatile uint32_t s_serial1_rx_tail = 0;
static volatile uint32_t s_serial1_rx_count = 0;
static int s_serial1_read_wait = 0;

static inline uint32_t serial1_irq_save(void) {
    uint32_t flags;
    asm volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void serial1_irq_restore(uint32_t flags) {
    asm volatile ("pushl %0; popfl" :: "r"(flags) : "memory");
}

static void serial1_callback(struct registers* regs) {
    (void) regs;
    int received = 0;
    while (inb(COM2 + 5) & 1) {
        uint8_t c = (uint8_t) inb(COM2);
        if (s_serial1_rx_count < SERIAL1_RX_BUF_SIZE) {
            s_serial1_rx_buf[s_serial1_rx_head] = c;
            s_serial1_rx_head = (s_serial1_rx_head + 1) % SERIAL1_RX_BUF_SIZE;
            s_serial1_rx_count++;
            received = 1;
        }
    }
    if (received) {
        scheduler_wake_channel(&s_serial1_read_wait);
    }
}

void serial1_init(void) {
    outb(COM2 + 1, 0x00);    /* Disable all interrupts initially */
    outb(COM2 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(COM2 + 0, 0x01);    /* Set divisor to 1 (115200 baud) */
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03);    /* 8 bits, no parity, one stop bit (8N1) */
    outb(COM2 + 2, 0xC7);    /* Enable FIFO, clear them, 14-byte threshold */
    outb(COM2 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */

    s_serial1_rx_head = 0;
    s_serial1_rx_tail = 0;
    s_serial1_rx_count = 0;
}

void serial1_install_irq(void) {
    register_interrupt_handler(35, &serial1_callback); /* IRQ3 = vector 35 */
    outb(COM2 + 1, 0x01);    /* Enable Received Data Available interrupt */
    outb(COM2 + 4, 0x0B);    /* IRQs enabled, OUT2 bit set */
}

int serial1_read(uint8_t* buf, uint32_t count, int nonblock) {
    if (count == 0) return 0;

    while (1) {
        uint32_t f = serial1_irq_save();
        if (s_serial1_rx_count > 0) {
            uint32_t n = 0;
            while (n < count && s_serial1_rx_count > 0) {
                buf[n++] = s_serial1_rx_buf[s_serial1_rx_tail];
                s_serial1_rx_tail = (s_serial1_rx_tail + 1) % SERIAL1_RX_BUF_SIZE;
                s_serial1_rx_count--;
            }
            serial1_irq_restore(f);
            return (int) n;
        }
        serial1_irq_restore(f);

        if (nonblock) return -EAGAIN;
        if (signal_has_deliverable(scheduler_current())) return -EINTR;
        scheduler_wait_channel(&s_serial1_read_wait);
        if (signal_has_deliverable(scheduler_current())) return -EINTR;
    }
}

int serial1_write(const uint8_t* buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        while ((inb(COM2 + 5) & 0x20) == 0);
        outb(COM2, buf[i]);
    }
    return (int) count;
}

short serial1_poll(short events, void** chan_out) {
    short revents = 0;
    if (events & POLLOUT) revents |= POLLOUT;
    if (events & POLLIN) {
        uint32_t f = serial1_irq_save();
        if (s_serial1_rx_count > 0) {
            revents |= POLLIN;
        } else if (chan_out && !*chan_out) {
            *chan_out = &s_serial1_read_wait;
        }
        serial1_irq_restore(f);
    }
    return revents;
}

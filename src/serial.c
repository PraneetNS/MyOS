#include "serial.h"
#include "io.h"
#include "vga.h"
#include <stdarg.h>

#define COM1 COM1_PORT

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* Disable all interrupts */
    outb(COM1 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x01);    /* Set divisor to 1 (115200 baud) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit (8N1) */
    outb(COM1 + 2, 0xC7);    /* Enable FIFO, clear them, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
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

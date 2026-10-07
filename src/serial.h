#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define COM1_PORT 0x3F8

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_printf(const char* fmt, ...);
void kprintf(const char* fmt, ...);

int serial_received(void);
char serial_read(void);
void serial_install_irq(void);

#endif

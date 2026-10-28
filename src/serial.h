#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_printf(const char* fmt, ...);
void kprintf(const char* fmt, ...);

int serial_received(void);
char serial_read(void);
void serial_install_irq(void);

/* COM2 (/dev/serial1) raw byte device */
void serial1_init(void);
void serial1_install_irq(void);
int serial1_read(uint8_t* buf, uint32_t count, int nonblock);
int serial1_write(const uint8_t* buf, uint32_t count);
short serial1_poll(short events, void** chan_out);

#endif

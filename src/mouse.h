/*
 * mouse.h - PS/2 mouse driver (Stage 18A Step 4)
 *
 * Initialises the PS/2 auxiliary device via the i8042 controller,
 * enables the mouse (default: stream mode, 3-byte packets), installs
 * IRQ12, and feeds imps2_packet_t events to input.c.
 */
#ifndef MOUSE_H
#define MOUSE_H

/* Initialise PS/2 mouse; must be called after idt_install(). */
void mouse_install(void);
void mouse_set_debug(int enable);

#endif /* MOUSE_H */

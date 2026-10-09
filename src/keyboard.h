#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_install(void);
void keyboard_handle_char(char c);
int keyboard_read_line(char* out, int maxlen); /* blocks until Enter is pressed */
uint32_t keyboard_get_modifiers(void);

#endif

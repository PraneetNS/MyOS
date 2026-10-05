#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_install(void);
void keyboard_handle_char(char c);
int keyboard_read_line(char* out, int maxlen); /* blocks until Enter is pressed */

#endif

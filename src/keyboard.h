#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_install(void);
int keyboard_read_line(char* out, int maxlen); /* blocks until Enter is pressed */

#endif

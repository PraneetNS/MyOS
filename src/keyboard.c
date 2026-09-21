#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "vga.h"

/* US QWERTY scancode set 1 -> ASCII, index = scancode, unshifted only for now */
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    /* rest unused for now: function keys, numpad, etc. */
};

static void keyboard_callback(struct registers* regs) {
    (void) regs;
    uint8_t scancode = inb(0x60);

    /* High bit set = key release; we only care about key press for now */
    if (scancode & 0x80)
        return;

    char c = (scancode < 128) ? scancode_ascii[scancode] : 0;
    if (c) {
        char buf[2] = { c, '\0' };
        terminal_writestring(buf);
    }
}

void keyboard_install(void) {
    register_interrupt_handler(33, &keyboard_callback); /* IRQ1 = vector 33 */
}

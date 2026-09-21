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

#define LINE_MAX 128
static char line_buf[LINE_MAX];
static volatile int line_len = 0;
static volatile int line_ready = 0;

static void keyboard_callback(struct registers* regs) {
    (void) regs;
    uint8_t scancode = inb(0x60);

    /* High bit set = key release; we only care about key press for now */
    if (scancode & 0x80)
        return;

    char c = (scancode < 128) ? scancode_ascii[scancode] : 0;
    if (!c) return;

    if (c == '\n') {
        line_buf[line_len] = '\0';
        terminal_putchar('\n');
        line_ready = 1;
    } else if (c == '\b') {
        if (line_len > 0) {
            line_len--;
            terminal_putchar('\b');
        }
    } else if (line_len < LINE_MAX - 1) {
        line_buf[line_len++] = c;
        terminal_putchar(c);
    }
}

void keyboard_install(void) {
    register_interrupt_handler(33, &keyboard_callback); /* IRQ1 = vector 33 */
}

int keyboard_read_line(char* out, int maxlen) {
    line_len = 0;
    line_ready = 0;

    while (!line_ready) asm volatile ("hlt");

    int n = line_len;
    if (n >= maxlen) n = maxlen - 1;
    for (int i = 0; i < n; i++) out[i] = line_buf[i];
    out[n] = '\0';
    return n;
}

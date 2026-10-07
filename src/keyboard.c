#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "tty.h"
#include "scheduler.h"

/* US QWERTY scancode set 1 -> ASCII */
static const char scancode_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ', 0,
    /* 0x3B-0x44: F1-F10 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    '7', '8', '9', '-',
    '4', '5', '6', '+',
    '1', '2', '3',
    '0', '.',
    0, 0, 0,
    0, 0  /* 0x57: F11, 0x58: F12 */
};

static const char scancode_shifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    '7', '8', '9', '-',
    '4', '5', '6', '+',
    '1', '2', '3',
    '0', '.',
    0, 0, 0,
    0, 0
};

static int shift_l = 0;
static int shift_r = 0;
static int ctrl_l = 0;
static int ctrl_r = 0;
static int alt_l = 0;
static int alt_r = 0;
static int caps_lock = 0;
static int e0_prefix = 0;

void keyboard_handle_char(char c) {
    tty_input_char(global_tty, c);
}

static void keyboard_callback(struct registers* regs) {
    (void) regs;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (e0_prefix) {
        e0_prefix = 0;

        if (sc & 0x80) {
            uint8_t rel = sc & 0x7F;
            if (rel == 0x1D) ctrl_r = 0;
            else if (rel == 0x38) alt_r = 0;
            return;
        }

        if (sc == 0x1D) { ctrl_r = 1; return; }
        if (sc == 0x38) { alt_r = 1; return; }

        /* Navigation and editing keys as ANSI escape sequences in raw mode */
        const char* seq = 0;
        switch (sc) {
            case 0x48: seq = "\033[A"; break; /* Up */
            case 0x50: seq = "\033[B"; break; /* Down */
            case 0x4D: seq = "\033[C"; break; /* Right */
            case 0x4B: seq = "\033[D"; break; /* Left */
            case 0x47: seq = "\033[H"; break; /* Home */
            case 0x4F: seq = "\033[F"; break; /* End */
            case 0x49: seq = "\033[5~"; break; /* Page Up */
            case 0x51: seq = "\033[6~"; break; /* Page Down */
            case 0x52: seq = "\033[2~"; break; /* Insert */
            case 0x53: seq = "\033[3~"; break; /* Delete */
            default: break;
        }

        if (seq && !tty_is_canonical(global_tty)) {
            tty_input_string(global_tty, seq);
        }
        return;
    }

    /* Key release */
    if (sc & 0x80) {
        uint8_t rel = sc & 0x7F;
        if (rel == 0x2A) shift_l = 0;
        else if (rel == 0x36) shift_r = 0;
        else if (rel == 0x1D) ctrl_l = 0;
        else if (rel == 0x38) alt_l = 0;
        return;
    }

    /* Key press */
    if (sc == 0x2A) { shift_l = 1; return; }
    if (sc == 0x36) { shift_r = 1; return; }
    if (sc == 0x1D) { ctrl_l = 1; return; }
    if (sc == 0x38) { alt_l = 1; return; }
    if (sc == 0x3A) { caps_lock = !caps_lock; return; }

    /* Function keys as ANSI sequences in raw mode */
    const char* fn_seq = 0;
    switch (sc) {
        case 0x3B: fn_seq = "\033OP"; break;   /* F1 */
        case 0x3C: fn_seq = "\033OQ"; break;   /* F2 */
        case 0x3D: fn_seq = "\033OR"; break;   /* F3 */
        case 0x3E: fn_seq = "\033OS"; break;   /* F4 */
        case 0x3F: fn_seq = "\033[15~"; break; /* F5 */
        case 0x40: fn_seq = "\033[17~"; break; /* F6 */
        case 0x41: fn_seq = "\033[18~"; break; /* F7 */
        case 0x42: fn_seq = "\033[19~"; break; /* F8 */
        case 0x43: fn_seq = "\033[20~"; break; /* F9 */
        case 0x44: fn_seq = "\033[21~"; break; /* F10 */
        case 0x57: fn_seq = "\033[23~"; break; /* F11 */
        case 0x58: fn_seq = "\033[24~"; break; /* F12 */
        default: break;
    }

    if (fn_seq) {
        if (!tty_is_canonical(global_tty)) {
            tty_input_string(global_tty, fn_seq);
        }
        return;
    }

    if (sc >= 128) return;

    int shift = shift_l || shift_r;
    int ctrl = ctrl_l || ctrl_r;

    char c = 0;
    if (ctrl) {
        char base = scancode_ascii[sc];
        if (base >= 'a' && base <= 'z') {
            c = (char)(base - 'a' + 1);
        } else if (base >= 'A' && base <= 'Z') {
            c = (char)(base - 'A' + 1);
        } else if (base == ' ') {
            c = 0;
        } else if (base == '[') {
            c = 27; /* ESC */
        } else if (base == '\\') {
            c = 28;
        } else if (base == ']') {
            c = 29;
        }
    } else {
        char unshifted = scancode_ascii[sc];
        if (unshifted >= 'a' && unshifted <= 'z') {
            int upper = shift ^ caps_lock;
            c = upper ? (char)(unshifted - 'a' + 'A') : unshifted;
        } else {
            c = shift ? scancode_shifted[sc] : unshifted;
        }
    }

    if (c) {
        tty_input_char(global_tty, c);
    }
}

void keyboard_install(void) {
    register_interrupt_handler(33, &keyboard_callback); /* IRQ1 = vector 33 */
}

int keyboard_read_line(char* out, int maxlen) {
    return tty_read(global_tty, out, maxlen);
}

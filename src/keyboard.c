#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "tty.h"
#include "console.h"
#include "input.h"
#include "scheduler.h"
#include "serial.h"

/* Linux input keycodes */
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_NUMLOCK     69
#define KEY_SCROLLLOCK  70
#define KEY_KP7         71
#define KEY_KP8         72
#define KEY_KP9         73
#define KEY_KPMINUS     74
#define KEY_KP4         75
#define KEY_KP5         76
#define KEY_KP6         77
#define KEY_KPPLUS      78
#define KEY_KP1         79
#define KEY_KP2         80
#define KEY_KP3         81
#define KEY_KP0         82
#define KEY_KPDOT       83
#define KEY_F11         87
#define KEY_F12         88
#define KEY_KPENTER     96
#define KEY_RIGHTCTRL   97
#define KEY_KPSLASH     98
#define KEY_RIGHTALT    100
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_INSERT      110
#define KEY_DELETE      111

#define KBD_MOD_LSHIFT  (1 << 0)
#define KBD_MOD_RSHIFT  (1 << 1)
#define KBD_MOD_LCTRL   (1 << 2)
#define KBD_MOD_RCTRL   (1 << 3)
#define KBD_MOD_LALT    (1 << 4)
#define KBD_MOD_RALT    (1 << 5)
#define KBD_MOD_CAPS    (1 << 6)
#define KBD_MOD_NUM     (1 << 7)

/* PS/2 Set 1 scancodes (0x00-0x58) to Linux KEY_* codes */
static const uint16_t scancode_to_keycode[128] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1,          [0x03] = KEY_2,          [0x04] = KEY_3,          [0x05] = KEY_4,
    [0x06] = KEY_5,          [0x07] = KEY_6,          [0x08] = KEY_7,          [0x09] = KEY_8,
    [0x0A] = KEY_9,          [0x0B] = KEY_0,          [0x0C] = KEY_MINUS,      [0x0D] = KEY_EQUAL,
    [0x0E] = KEY_BACKSPACE,  [0x0F] = KEY_TAB,
    [0x10] = KEY_Q,          [0x11] = KEY_W,          [0x12] = KEY_E,          [0x13] = KEY_R,
    [0x14] = KEY_T,          [0x15] = KEY_Y,          [0x16] = KEY_U,          [0x17] = KEY_I,
    [0x18] = KEY_O,          [0x19] = KEY_P,          [0x1A] = KEY_LEFTBRACE,  [0x1B] = KEY_RIGHTBRACE,
    [0x1C] = KEY_ENTER,      [0x1D] = KEY_LEFTCTRL,
    [0x1E] = KEY_A,          [0x1F] = KEY_S,          [0x20] = KEY_D,          [0x21] = KEY_F,
    [0x22] = KEY_G,          [0x23] = KEY_H,          [0x24] = KEY_J,          [0x25] = KEY_K,
    [0x26] = KEY_L,          [0x27] = KEY_SEMICOLON,  [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,
    [0x2A] = KEY_LEFTSHIFT,  [0x2B] = KEY_BACKSLASH,
    [0x2C] = KEY_Z,          [0x2D] = KEY_X,          [0x2E] = KEY_C,          [0x2F] = KEY_V,
    [0x30] = KEY_B,          [0x31] = KEY_N,          [0x32] = KEY_M,          [0x33] = KEY_COMMA,
    [0x34] = KEY_DOT,        [0x35] = KEY_SLASH,      [0x36] = KEY_RIGHTSHIFT, [0x37] = KEY_KPASTERISK,
    [0x38] = KEY_LEFTALT,    [0x39] = KEY_SPACE,      [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1,         [0x3C] = KEY_F2,         [0x3D] = KEY_F3,         [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,         [0x40] = KEY_F6,         [0x41] = KEY_F7,         [0x42] = KEY_F8,
    [0x43] = KEY_F9,         [0x44] = KEY_F10,
    [0x45] = KEY_NUMLOCK,    [0x46] = KEY_SCROLLLOCK,
    [0x47] = KEY_KP7,        [0x48] = KEY_KP8,        [0x49] = KEY_KP9,        [0x4A] = KEY_KPMINUS,
    [0x4B] = KEY_KP4,        [0x4C] = KEY_KP5,        [0x4D] = KEY_KP6,        [0x4E] = KEY_KPPLUS,
    [0x4F] = KEY_KP1,        [0x50] = KEY_KP2,        [0x51] = KEY_KP3,        [0x52] = KEY_KP0,
    [0x53] = KEY_KPDOT,
    [0x57] = KEY_F11,        [0x58] = KEY_F12,
};

static uint16_t e0_to_keycode(uint8_t sc) {
    switch (sc) {
        case 0x1C: return KEY_KPENTER;
        case 0x1D: return KEY_RIGHTCTRL;
        case 0x35: return KEY_KPSLASH;
        case 0x38: return KEY_RIGHTALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGEUP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        default:   return KEY_RESERVED;
    }
}

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

uint32_t keyboard_get_modifiers(void) {
    uint32_t mods = 0;
    if (shift_l)   mods |= KBD_MOD_LSHIFT;
    if (shift_r)   mods |= KBD_MOD_RSHIFT;
    if (ctrl_l)    mods |= KBD_MOD_LCTRL;
    if (ctrl_r)    mods |= KBD_MOD_RCTRL;
    if (alt_l)     mods |= KBD_MOD_LALT;
    if (alt_r)     mods |= KBD_MOD_RALT;
    if (caps_lock) mods |= KBD_MOD_CAPS;
    return mods;
}

void keyboard_handle_char(char c) {
    tty_input_char(global_tty, c);
}

static void keyboard_callback(struct registers* regs) {
    (void) regs;

    /* Status bit 5 (AUXB): if set, data is for the mouse, not keyboard */
    uint8_t stat = inb(0x64);
    if (stat & 0x20) {
        return;
    }

    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    uint16_t keycode = KEY_RESERVED;
    int is_release = (sc & 0x80) != 0;
    uint8_t clean_sc = sc & 0x7F;

    if (e0_prefix) {
        e0_prefix = 0;
        keycode = e0_to_keycode(clean_sc);
        if (clean_sc == 0x1D) ctrl_r = !is_release;
        else if (clean_sc == 0x38) alt_r = !is_release;
    } else {
        if (clean_sc < 128) {
            keycode = scancode_to_keycode[clean_sc];
        }
        if (clean_sc == 0x2A) shift_l = !is_release;
        else if (clean_sc == 0x36) shift_r = !is_release;
        else if (clean_sc == 0x1D) ctrl_l = !is_release;
        else if (clean_sc == 0x38) alt_l = !is_release;
        else if (clean_sc == 0x3A && !is_release) caps_lock = !caps_lock;
    }

    /* Emergency console restore: Ctrl + Alt + F1 restores K_XLATE */
    if (!is_release && (ctrl_l || ctrl_r) && (alt_l || alt_r) && keycode == KEY_F1) {
        console_set_kbmode(K_XLATE);
        serial_printf("[keyboard] Restored K_XLATE via Ctrl+Alt+F1\n");
    }

    /* Always deliver raw key event to /dev/input/kbd0 */
    if (keycode != KEY_RESERVED) {
        input_push_key(keycode, is_release ? 0 : 1);
    }

    /* In K_OFF mode, TTY ignores keyboard input */
    if (console_get_kbmode() == K_OFF) {
        return;
    }

    /* ------------------------------------------------------------------ */
    /* Normal TTY processing (K_XLATE mode)                                */
    /* ------------------------------------------------------------------ */

    if (sc & 0x80) {
        /* Release, nothing more for TTY */
        return;
    }

    /* Navigation / editing keys for TTY in raw mode */
    const char* seq = 0;
    switch (clean_sc) {
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

    if (seq) {
        if (!tty_is_canonical(global_tty)) {
            tty_input_string(global_tty, seq);
        }
        return;
    }

    /* Function keys as ANSI sequences in raw mode */
    const char* fn_seq = 0;
    switch (clean_sc) {
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

    if (clean_sc >= 128) return;

    int shift = shift_l || shift_r;
    int ctrl  = ctrl_l || ctrl_r;

    char c = 0;
    if (ctrl) {
        char base = scancode_ascii[clean_sc];
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
        char unshifted = scancode_ascii[clean_sc];
        if (unshifted >= 'a' && unshifted <= 'z') {
            int upper = shift ^ caps_lock;
            c = upper ? (char)(unshifted - 'a' + 'A') : unshifted;
        } else {
            c = shift ? scancode_shifted[clean_sc] : unshifted;
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

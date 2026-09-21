#include "usermode_demo.h"

/* This function's CODE runs at CPL=3 (ring 3) once jumped to via
   enter_usermode(). It must not call kernel C functions directly --
   the whole point is that the only way out is `int 0x80`. */

static inline void sys_write(const char* s) {
    asm volatile ("int $0x80" : : "a"(1), "b"(s));
}

static inline void sys_exit(void) {
    asm volatile ("int $0x80" : : "a"(0));
}

void usermode_demo_entry(void) {
    sys_write("[ring 3] Hello from user mode! CPL=3, reached only via iret.\n");
    sys_write("[ring 3] This string was printed by asking the kernel via int 0x80 --\n");
    sys_write("[ring 3] ring 3 code has no direct access to the VGA driver.\n");
    sys_exit();

    /* sys_exit hands off to the scheduler and never returns here --
       this loop only exists as a safety net in case it somehow did. */
    for (;;) { }
}

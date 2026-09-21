#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

/* Jumps to entry_point in ring 3, running on user_stack_top.
   Never returns -- the user code returns to the kernel only via syscalls. */
void enter_usermode(uint32_t entry_point, uint32_t user_stack_top);

#endif

#ifndef TASK_H
#define TASK_H

#include <stdint.h>

void task_create(void (*entry)(void));
void scheduler_start(void);       /* never returns */
void scheduler_tick(void);        /* called from the timer IRQ */

#endif

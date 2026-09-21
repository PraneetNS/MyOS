#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

/* Starts the scheduler by switching into process 0 (the shell). Call
   once, after process_init_table(). Never returns. */
void scheduler_start(void);

/* Called from the timer IRQ; no-op until scheduler_start() has run. */
void scheduler_tick(void);

/* Called from a process's sys_exit (or a fatal page fault): tears the
   current process down via process_destroy() and switches to whatever
   process is next in the round-robin rotation (always at least the
   shell, which never exits). Never returns. */
void scheduler_exit_current(void);

/* The process currently executing -- used by process.c's trampoline to
   find its own entry point/stack on a brand-new process's first run. */
process_t* scheduler_current(void);

#endif

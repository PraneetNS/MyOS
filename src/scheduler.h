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

/* Called from sys_spawn_wait's syscall handler after spawning a child:
   marks the calling process WAITING (so the scheduler won't pick it
   again) and switches away immediately. Returns once
   scheduler_exit_current() wakes it back up when child_pid exits -- at
   that point this looks, to the caller, just like an ordinary function
   return. */
void scheduler_wait_for(int child_pid);

/* Stage 9: same blocking mechanism as scheduler_wait_for(), but for a
   process waiting on pipe data rather than a specific child's exit
   (see src/pipe.c). */
void scheduler_wait_for_pipe(void);
void scheduler_wake_pipe_waiters(void);

#endif

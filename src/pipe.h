#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

/* A deliberately simple IPC primitive: ONE global, fixed-size ring
   buffer in the kernel, not a general pipe() syscall returning fd
   pairs. A real OS's pipes are per-pipe-instance and integrate with
   the fd table (see SYS_OPEN/READ/CLOSE in syscall.c for what that
   would look like); this is the simplest thing that demonstrates real
   blocking producer/consumer IPC between processes without that
   additional plumbing. */

void pipe_init(void);

/* Writes `len` bytes into the pipe. Never blocks -- if the ring buffer
   fills up, remaining bytes are simply dropped (documented limitation;
   a real implementation would block the writer too). Returns bytes
   actually written. Wakes any process blocked in pipe_read(). */
uint32_t pipe_write(const uint8_t* data, uint32_t len);

/* Reads up to `maxlen` bytes from the pipe into `buf`. If the pipe is
   currently empty, BLOCKS the calling process (via the scheduler) until
   a writer provides data, then returns whatever became available.
   Returns bytes read (always > 0 -- this call blocks rather than ever
   returning 0/EOF, since the pipe never "closes"). */
uint32_t pipe_read(uint8_t* buf, uint32_t maxlen);

#endif

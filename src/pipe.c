#include "pipe.h"
#include "scheduler.h"

#define PIPE_SIZE 256

static uint8_t ring[PIPE_SIZE];
static uint32_t head = 0; /* next write position */
static uint32_t tail = 0; /* next read position */
static uint32_t count = 0; /* bytes currently buffered */

void pipe_init(void) {
    head = tail = count = 0;
}

uint32_t pipe_write(const uint8_t* data, uint32_t len) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (count >= PIPE_SIZE) break; /* full -- drop the rest (documented limitation) */
        ring[head] = data[i];
        head = (head + 1) % PIPE_SIZE;
        count++;
        written++;
    }
    if (written > 0) scheduler_wake_pipe_waiters();
    return written;
}

uint32_t pipe_read(uint8_t* buf, uint32_t maxlen) {
    while (count == 0) {
        scheduler_wait_for_pipe(); /* blocks; loop re-checks in case of a spurious wake */
    }

    uint32_t n = 0;
    while (n < maxlen && count > 0) {
        buf[n++] = ring[tail];
        tail = (tail + 1) % PIPE_SIZE;
        count--;
    }
    return n;
}

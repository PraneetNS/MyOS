#include "pipe.h"
#include "scheduler.h"
#include "kheap.h"
#include "serial.h"
#include "vga.h"
#include "errno.h"
#include "signal.h"

static int active_pipe_count = 0;

void pipe_init(void) {
    active_pipe_count = 0;
}

int pipe_get_active_count(void) {
    return active_pipe_count;
}

int pipe_create(pipe_t** out_pipe) {
    if (!out_pipe) return -EINVAL;

    pipe_t* p = (pipe_t*) kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;

    p->buffer = (uint8_t*) kmalloc(PIPE_CAPACITY);
    if (!p->buffer) {
        kfree(p);
        return -ENOMEM;
    }

    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->reader_count = 1;
    p->writer_count = 1;
    p->read_wait = 0;
    p->write_wait = 0;

    active_pipe_count++;
    *out_pipe = p;
    return 0;
}

int pipe_read(pipe_t* p, void* buf, uint32_t count) {
    if (!p) return -EBADF;
    if (count == 0) return 0;

    uint8_t* dst = (uint8_t*) buf;
    uint32_t read_bytes = 0;

    while (p->count == 0) {
        if (p->writer_count == 0) {
            return 0; /* EOF */
        }
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
        scheduler_wait_channel(&p->read_wait);
        if (signal_has_deliverable(scheduler_current())) {
            return -EINTR;
        }
    }

    while (read_bytes < count && p->count > 0) {
        dst[read_bytes++] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % PIPE_CAPACITY;
        p->count--;
    }

    /* Buffer now has space: wake any blocked writers */
    scheduler_wake_channel(&p->write_wait);

    return (int) read_bytes;
}

int pipe_write(pipe_t* p, const void* buf, uint32_t count) {
    if (!p) return -EBADF;
    if (count == 0) return 0;

    const uint8_t* src = (const uint8_t*) buf;
    uint32_t written = 0;

    while (written < count) {
        if (p->reader_count == 0) {
            sig_send(scheduler_current(), SIGPIPE);
            if (written > 0) return (int) written;
            return -EPIPE;
        }

        while (p->count == PIPE_CAPACITY) {
            if (signal_has_deliverable(scheduler_current())) {
                return (written > 0) ? (int) written : -EINTR;
            }
            scheduler_wait_channel(&p->write_wait);
            if (p->reader_count == 0) {
                sig_send(scheduler_current(), SIGPIPE);
                if (written > 0) return (int) written;
                return -EPIPE;
            }
            if (signal_has_deliverable(scheduler_current())) {
                return (written > 0) ? (int) written : -EINTR;
            }
        }

        while (written < count && p->count < PIPE_CAPACITY) {
            p->buffer[p->head] = src[written++];
            p->head = (p->head + 1) % PIPE_CAPACITY;
            p->count++;
        }

        scheduler_wake_channel(&p->read_wait);
    }

    return (int) written;
}

void pipe_close_end(pipe_t* p, int flags) {
    if (!p) return;

    if ((flags & 3) == O_WRONLY) {
        p->writer_count--;
        if (p->writer_count < 0) {
            kprintf("[pipe] ASSERTION FAILED: pipe %p writer_count negative: %d\n", p, p->writer_count);
        }
        /* Wake any blocked readers so they see EOF or remaining data */
        scheduler_wake_channel(&p->read_wait);
    } else {
        p->reader_count--;
        if (p->reader_count < 0) {
            kprintf("[pipe] ASSERTION FAILED: pipe %p reader_count negative: %d\n", p, p->reader_count);
        }
        /* Wake any blocked writers so they receive -EPIPE */
        scheduler_wake_channel(&p->write_wait);
    }

    if (p->reader_count <= 0 && p->writer_count <= 0) {
        if (p->buffer) {
            kfree(p->buffer);
            p->buffer = 0;
        }
        kfree(p);
        active_pipe_count--;
        if (active_pipe_count < 0) {
            kprintf("[pipe] ASSERTION FAILED: active_pipe_count negative: %d\n", active_pipe_count);
        }
    }
}

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_CAPACITY 4096

typedef struct pipe {
    uint8_t* buffer;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int reader_count;
    int writer_count;
    int read_wait;   /* address used as wait channel for blocked readers */
    int write_wait;  /* address used as wait channel for blocked writers */
} pipe_t;

void pipe_init(void);
int pipe_create(pipe_t** out_pipe);
int pipe_read(pipe_t* p, void* buf, uint32_t count);
int pipe_write(pipe_t* p, const void* buf, uint32_t count);
void pipe_close_end(pipe_t* p, int flags);
int pipe_get_active_count(void);

#endif

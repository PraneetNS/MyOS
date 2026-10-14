#ifndef KMUTEX_H
#define KMUTEX_H

#include <stdint.h>
#include "process.h"

#define KMUTEX_MAX_WAITERS 32

typedef struct kmutex {
    int locked;
    int owner_pid;
    int wait_head;
    int wait_tail;
    int wait_count;
    process_t* waiters[KMUTEX_MAX_WAITERS];
    const char* name;
} kmutex_t;

void kmutex_init(kmutex_t* m, const char* name);
void kmutex_lock(kmutex_t* m);
void kmutex_unlock(kmutex_t* m);
int kmutex_is_locked(const kmutex_t* m);
int kmutex_owner(const kmutex_t* m);

/* Debug / sanity assertions: panic if current process holds any mutex */
void kmutex_assert_none_held(void);

#endif

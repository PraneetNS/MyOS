#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

/* Futex operations */
#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_WAIT_REQUEUE_PI   11
#define FUTEX_CMP_REQUEUE_PI    12

#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_WAITERS           0x80000000U
#define FUTEX_OWNER_DIED        0x40000000U
#define FUTEX_TID_MASK          0x3FFFFFFFU
#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFFU

struct process;
struct timespec;

void futex_init(void);
int sys_futex(uint32_t* uaddr, int op, uint32_t val, const struct timespec* timeout, uint32_t* uaddr2, uint32_t val3);
int futex_wake(uint32_t* uaddr, int count, uint32_t bitset);
void futex_handle_thread_exit(struct process* p);

#endif


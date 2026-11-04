#include "futex.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "paging.h"
#include "serial.h"
#include <errno.h>

#define FUTEX_HASH_SIZE 64
#define MAX_FUTEX_WAITERS 256

typedef struct futex_waiter {
    struct process* task;
    uint32_t* uaddr;
    uint64_t as_pml4;
    uint32_t bitset;
    int active;
    int woken;
    struct futex_waiter* next;
} futex_waiter_t;

static futex_waiter_t waiter_pool[MAX_FUTEX_WAITERS];
static futex_waiter_t* futex_buckets[FUTEX_HASH_SIZE];

static inline uint32_t futex_hash(uint32_t* uaddr, uint64_t as_pml4) {
    uint64_t v = ((uint64_t)uaddr) ^ (as_pml4 >> 12);
    v = (~v) + (v << 18);
    v = v ^ (v >> 31);
    v = v * 21;
    v = v ^ (v >> 11);
    v = v + (v << 6);
    v = v ^ (v >> 22);
    return (uint32_t)(v % FUTEX_HASH_SIZE);
}

void futex_init(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        futex_buckets[i] = 0;
    }
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        waiter_pool[i].active = 0;
        waiter_pool[i].next = 0;
    }
}

static futex_waiter_t* alloc_waiter(void) {
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!waiter_pool[i].active) {
            waiter_pool[i].active = 1;
            waiter_pool[i].woken = 0;
            waiter_pool[i].next = 0;
            return &waiter_pool[i];
        }
    }
    return 0;
}

static void free_waiter(futex_waiter_t* w) {
    if (w) {
        w->active = 0;
        w->task = 0;
        w->next = 0;
    }
}

int futex_wake(uint32_t* uaddr, int count, uint32_t bitset) {
    if (!uaddr || count <= 0) return 0;
    process_t* cur = scheduler_current();
    if (!cur) return 0;
    uint64_t as_pml4 = cur->as.pml4;

    uint32_t h = futex_hash(uaddr, as_pml4);
    futex_waiter_t** curr = &futex_buckets[h];
    int woken_count = 0;

    while (*curr && woken_count < count) {
        futex_waiter_t* w = *curr;
        if (w->uaddr == uaddr && w->as_pml4 == as_pml4 && (w->bitset & bitset)) {
            *curr = w->next;
            w->woken = 1;
            if (w->task) {
                timer_sleep_dequeue(w->task);
                w->task->state = PROC_READY;
                w->task->wait_channel = 0;
            }
            woken_count++;
        } else {
            curr = &(*curr)->next;
        }
    }

    return woken_count;
}

int sys_futex(uint32_t* uaddr, int op, uint32_t val, const struct timespec* timeout, uint32_t* uaddr2, uint32_t val3) {
    if (!uaddr) return -EINVAL;
    if ((uintptr_t)uaddr & 3) return -EINVAL; /* 4-byte aligned */

    int cmd = op & FUTEX_CMD_MASK;
    process_t* cur = scheduler_current();
    if (!cur) return -ESRCH;

    uint64_t as_pml4 = cur->as.pml4;

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        uint32_t bitset = (cmd == FUTEX_WAIT_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -EINVAL;

        int err = validate_user_buffer(uaddr, sizeof(uint32_t), 0);
        if (err != 0) return err;

        uint32_t cur_val = 0;
        if (copy_from_user(&cur_val, uaddr, sizeof(uint32_t)) != 0) {
            return -EFAULT;
        }
        if (cur_val != val) {
            return -EAGAIN;
        }

        futex_waiter_t* w = alloc_waiter();
        if (!w) return -ENOMEM;

        w->task = cur;
        w->uaddr = uaddr;
        w->as_pml4 = as_pml4;
        w->bitset = bitset;
        w->woken = 0;

        uint32_t h = futex_hash(uaddr, as_pml4);
        w->next = futex_buckets[h];
        futex_buckets[h] = w;

        cur->state = PROC_WAITING;
        cur->wait_channel = uaddr;

        int has_timeout = 0;
        uint32_t deadline = 0;
        if (timeout != 0) {
            struct timespec ts;
            if (copy_from_user(&ts, timeout, sizeof(ts)) == 0) {
                if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
                    /* Remove from bucket and free */
                    futex_waiter_t** pcur = &futex_buckets[h];
                    while (*pcur) {
                        if (*pcur == w) { *pcur = w->next; break; }
                        pcur = &(*pcur)->next;
                    }
                    free_waiter(w);
                    cur->state = PROC_READY;
                    cur->wait_channel = 0;
                    return -EINVAL;
                }
                uint32_t delta_ticks = (uint32_t)((ts.tv_sec * 100) + (ts.tv_nsec / 10000000L));
                if (op & FUTEX_CLOCK_REALTIME) {
                    /* Absolute timeout */
                    deadline = delta_ticks;
                } else {
                    /* Relative timeout */
                    deadline = timer_get_ticks() + delta_ticks;
                }
                has_timeout = 1;
                timer_sleep_enqueue(cur, deadline);
            }
        }

        while (!w->woken) {
            if (has_timeout && timer_get_ticks() >= deadline) {
                break;
            }
            if (cur->sig_pending & ~cur->sig_blocked) {
                break;
            }
            scheduler_yield();
        }

        if (has_timeout) {
            timer_sleep_dequeue(cur);
        }

        /* Unlink if still present in bucket */
        futex_waiter_t** pcur = &futex_buckets[h];
        while (*pcur) {
            if (*pcur == w) {
                *pcur = w->next;
                break;
            }
            pcur = &(*pcur)->next;
        }

        int ret = 0;
        if (w->woken) {
            ret = 0;
        } else if (has_timeout && timer_get_ticks() >= deadline) {
            ret = -ETIMEDOUT;
        } else if (cur->sig_pending & ~cur->sig_blocked) {
            ret = -EINTR;
        }

        free_waiter(w);
        cur->state = PROC_READY;
        cur->wait_channel = 0;
        return ret;
    }

    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        uint32_t bitset = (cmd == FUTEX_WAKE_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -EINVAL;
        return futex_wake(uaddr, (int)val, bitset);
    }

    if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE) {
        if (!uaddr2 || ((uintptr_t)uaddr2 & 3)) return -EINVAL;

        int err = validate_user_buffer(uaddr, sizeof(uint32_t), 0);
        if (err != 0) return err;

        if (cmd == FUTEX_CMP_REQUEUE) {
            uint32_t cur_val = 0;
            if (copy_from_user(&cur_val, uaddr, sizeof(uint32_t)) != 0) {
                return -EFAULT;
            }
            if (cur_val != val3) {
                return -EAGAIN;
            }
        }

        int woken = futex_wake(uaddr, (int)val, FUTEX_BITSET_MATCH_ANY);

        /* Requeue remaining up to (uint32_t)(uintptr_t)timeout */
        uint32_t max_requeue = (uint32_t)(uintptr_t)timeout;
        uint32_t requeued = 0;

        uint32_t h1 = futex_hash(uaddr, as_pml4);
        uint32_t h2 = futex_hash(uaddr2, as_pml4);

        futex_waiter_t** curr = &futex_buckets[h1];
        while (*curr && requeued < max_requeue) {
            futex_waiter_t* w = *curr;
            if (w->uaddr == uaddr && w->as_pml4 == as_pml4) {
                *curr = w->next;
                w->uaddr = uaddr2;
                if (w->task) w->task->wait_channel = uaddr2;
                w->next = futex_buckets[h2];
                futex_buckets[h2] = w;
                requeued++;
            } else {
                curr = &(*curr)->next;
            }
        }

        return (int)(woken + requeued);
    }

    return -ENOSYS;
}

struct robust_list {
    struct robust_list* next;
};

struct robust_list_head {
    struct robust_list list;
    long futex_offset;
    struct robust_list* list_op_pending;
};

void futex_handle_thread_exit(process_t* p) {
    if (!p) return;

    /* 1. Clear child TID if registered */
    if (p->clear_child_tid) {
        uint32_t zero = 0;
        copy_to_user(p->clear_child_tid, &zero, sizeof(zero));
        futex_wake((uint32_t*)p->clear_child_tid, 1, FUTEX_BITSET_MATCH_ANY);
        p->clear_child_tid = 0;
    }

    /* 2. Walk robust futex list if set */
    if (p->robust_list_head) {
        struct robust_list_head head;
        if (copy_from_user(&head, p->robust_list_head, sizeof(head)) == 0) {
            struct robust_list* entry = head.list.next;
            int limit = 128; /* Bound loop to avoid infinite loops on circular user structures */
            while (entry && entry != (struct robust_list*)p->robust_list_head && limit-- > 0) {
                uint32_t* futex_word = (uint32_t*)((char*)entry + head.futex_offset);
                uint32_t val = 0;
                if (copy_from_user(&val, futex_word, sizeof(val)) == 0) {
                    if ((val & FUTEX_TID_MASK) == (uint32_t)p->tid) {
                        uint32_t new_val = (val & ~FUTEX_TID_MASK) | FUTEX_OWNER_DIED;
                        copy_to_user(futex_word, &new_val, sizeof(new_val));
                        futex_wake(futex_word, 1, FUTEX_BITSET_MATCH_ANY);
                    }
                }
                struct robust_list next_entry;
                if (copy_from_user(&next_entry, entry, sizeof(next_entry)) != 0) break;
                entry = next_entry.next;
            }
        }
        p->robust_list_head = 0;
    }
}

#include "kmutex.h"
#include "vga.h"
#include "serial.h"
#include "scheduler.h"

#define MAX_REGISTERED_MUTEXES 16
static kmutex_t* registered_mutexes[MAX_REGISTERED_MUTEXES];
static int registered_count = 0;

static void kmutex_register(kmutex_t* m) {
    uint32_t flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags));
    for (int i = 0; i < registered_count; i++) {
        if (registered_mutexes[i] == m) {
            asm volatile ("push %0; popf" : : "r"(flags));
            return;
        }
    }
    if (registered_count < MAX_REGISTERED_MUTEXES) {
        registered_mutexes[registered_count++] = m;
    }
    asm volatile ("push %0; popf" : : "r"(flags));
}

static void kmutex_panic(const char* msg, kmutex_t* m) {
    terminal_writestring("\n*** KERNEL PANIC [kmutex]: ");
    terminal_writestring(msg);
    if (m && m->name) {
        terminal_writestring(" (mutex: ");
        terminal_writestring(m->name);
        terminal_writestring(")");
    }
    terminal_writestring(" ***\nSystem halted.\n");

    serial_printf("\n*** KERNEL PANIC [kmutex]: %s (mutex: %s, locked=%d, owner=%d) ***\nSystem halted.\n",
                  msg, (m && m->name) ? m->name : "none",
                  m ? m->locked : -1, m ? m->owner_pid : -1);

    asm volatile ("cli");
    for (;;) asm volatile ("hlt");
}

void kmutex_init(kmutex_t* m, const char* name) {
    if (!m) return;
    m->locked = 0;
    m->owner_pid = 0;
    m->wait_head = 0;
    m->wait_tail = 0;
    m->wait_count = 0;
    m->name = name ? name : "unnamed";
    for (int i = 0; i < KMUTEX_MAX_WAITERS; i++) {
        m->waiters[i] = 0;
    }
    kmutex_register(m);
}

void kmutex_lock(kmutex_t* m) {
    if (!m) return;

    process_t* me = scheduler_current();
    int my_pid = me ? me->pid : 0;

    for (;;) {
        uint32_t flags;
        asm volatile ("pushf; pop %0; cli" : "=r"(flags));

        /* If unlocked or directly handed off to us, claim lock */
        if (!m->locked || (m->owner_pid == my_pid && me != 0)) {
            m->locked = 1;
            m->owner_pid = my_pid;
            asm volatile ("push %0; popf" : : "r"(flags));
            return;
        }

        /* Detect recursive lock deadlock */
        if (m->owner_pid == my_pid && me != 0) {
            asm volatile ("push %0; popf" : : "r"(flags));
            kmutex_panic("recursive lock attempt on non-recursive mutex", m);
            return;
        }

        /* Enqueue calling process onto wait queue */
        if (m->wait_count >= KMUTEX_MAX_WAITERS) {
            asm volatile ("push %0; popf" : : "r"(flags));
            kmutex_panic("wait queue capacity exceeded", m);
            return;
        }

        m->waiters[m->wait_tail] = me;
        m->wait_tail = (m->wait_tail + 1) % KMUTEX_MAX_WAITERS;
        m->wait_count++;

        if (me) {
            me->state = PROC_WAITING;
            me->wait_channel = m;
        }

        /* Switch away to next ready task. Interrupts will be restored when incoming task runs */
        scheduler_wait_or_idle(me);

        if (me) {
            me->wait_channel = 0;
        }

        asm volatile ("push %0; popf" : : "r"(flags));

        /* If direct handoff assigned lock to us while sleeping, return */
        if (m->locked && m->owner_pid == my_pid && me != 0) {
            return;
        }
    }
}

void kmutex_unlock(kmutex_t* m) {
    if (!m) return;

    uint32_t flags;
    asm volatile ("pushf; pop %0; cli" : "=r"(flags));

    process_t* me = scheduler_current();
    int my_pid = me ? me->pid : 0;

    if (!m->locked) {
        asm volatile ("push %0; popf" : : "r"(flags));
        kmutex_panic("attempt to unlock unlocked mutex", m);
        return;
    }

    if (me != 0 && m->owner_pid != my_pid) {
        asm volatile ("push %0; popf" : : "r"(flags));
        kmutex_panic("attempt to unlock mutex owned by different PID", m);
        return;
    }

    /* Wake next valid waiter in FIFO order */
    while (m->wait_count > 0) {
        process_t* waiter = m->waiters[m->wait_head];
        m->wait_head = (m->wait_head + 1) % KMUTEX_MAX_WAITERS;
        m->wait_count--;

        if (waiter && waiter->state != PROC_UNUSED && waiter->state != PROC_ZOMBIE) {
            /* Direct handoff: transfer lock ownership to waiter and wake it up */
            m->locked = 1;
            m->owner_pid = waiter->pid;
            waiter->state = PROC_READY;
            waiter->wait_channel = 0;
            asm volatile ("push %0; popf" : : "r"(flags));
            return;
        }
    }

    /* No waiting processes: release lock */
    m->locked = 0;
    m->owner_pid = 0;
    asm volatile ("push %0; popf" : : "r"(flags));
}

int kmutex_is_locked(const kmutex_t* m) {
    return m ? m->locked : 0;
}

int kmutex_owner(const kmutex_t* m) {
    return m ? m->owner_pid : 0;
}

void kmutex_assert_none_held(void) {
    process_t* me = scheduler_current();
    if (!me || me->is_kernel_task) return;
    int my_pid = me->pid;

    for (int i = 0; i < registered_count; i++) {
        kmutex_t* m = registered_mutexes[i];
        if (m && m->locked && m->owner_pid == my_pid) {
            terminal_writestring("\n*** KERNEL PANIC [kmutex]: mutex held across syscall return or at exit! ***\n");
            kmutex_panic("mutex held at syscall return or process exit", m);
        }
    }
}

#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "process.h"
#include "fs.h"
#include "kheap.h"

#define SYS_EXIT       0
#define SYS_WRITE      1
#define SYS_GETPID     2
#define SYS_OPEN       3
#define SYS_READ       4
#define SYS_CLOSE      5
#define SYS_SPAWN_WAIT 6

extern void isr128(void); /* defined in isr.s */

/* Spawns `name` as a new child of the calling process and reads its
   file in from disk -- the same steps shell.c's cmd_run performs, just
   triggered by a process instead of the shell. */
static process_t* spawn_child(const char* name, int parent_pid) {
    const fs_entry_t* e = fs_find(name);
    if (!e) return 0;

    uint32_t alloc_size = ((e->size_bytes + 511) / 512) * 512;
    uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
    if (!buf) return 0;

    int n = fs_read_file(e, buf);
    if (n < 0) { kfree(buf); return 0; }

    process_t* child = process_spawn_from_elf(name, buf, (uint32_t) n, parent_pid);
    kfree(buf);
    return child;
}

static void syscall_handler(struct registers* regs) {
    switch (regs->eax) {
        case SYS_WRITE:
            /* regs->ebx = pointer to a NUL-terminated string, set by the
               caller in EBX before `int 0x80` (see userland/libc.h) */
            terminal_writestring((const char*) regs->ebx);
            break;

        case SYS_EXIT:
            terminal_writestring("[ok] process exited\n");
            /* Tears the process down and switch_task()s into whatever's
               next in the round-robin rotation. Because this goes
               through switch_task's popf (not a bare function call),
               interrupts get correctly re-enabled for whatever we
               resume -- no manual `sti` needed here. */
            scheduler_exit_current(); /* never returns */
            break;

        case SYS_GETPID:
            regs->eax = (uint32_t) scheduler_current()->pid;
            break;

        case SYS_OPEN: {
            const char* name = (const char*) regs->ebx;
            const fs_entry_t* e = fs_find(name);
            if (!e) { regs->eax = (uint32_t)-1; break; }

            process_t* me = scheduler_current();
            int slot = -1;
            for (int i = 0; i < MAX_FDS; i++) if (!me->fds[i].in_use) { slot = i; break; }
            if (slot < 0) { regs->eax = (uint32_t)-1; break; }

            me->fds[slot].entry = e;
            me->fds[slot].offset = 0;
            me->fds[slot].in_use = 1;
            regs->eax = (uint32_t) slot;
            break;
        }

        case SYS_READ: {
            int fd = (int) regs->ebx;
            uint8_t* buf = (uint8_t*) regs->ecx;
            uint32_t len = regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd].in_use) { regs->eax = (uint32_t)-1; break; }

            int n = fs_read_range(me->fds[fd].entry, me->fds[fd].offset, buf, len);
            if (n > 0) me->fds[fd].offset += (uint32_t) n;
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_CLOSE: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd >= 0 && fd < MAX_FDS) me->fds[fd].in_use = 0;
            regs->eax = 0;
            break;
        }

        case SYS_SPAWN_WAIT: {
            const char* name = (const char*) regs->ebx;
            process_t* me = scheduler_current();

            process_t* child = spawn_child(name, me->pid);
            if (!child) { regs->eax = (uint32_t)-1; break; }

            /* This return value is baked into `me`'s saved register
               frame now, ready for whenever it's eventually resumed --
               see scheduler_wait_for()'s doc comment. */
            regs->eax = (uint32_t) child->pid;
            scheduler_wait_for(child->pid); /* blocks; returns once child exits */
            break;
        }

        default:
            terminal_writestring("[warn] unknown syscall\n");
            break;
    }
}

void syscall_install(void) {
    /* 0xEE = present, DPL=3 (so ring-3 code is allowed to `int 0x80`
       without a general protection fault), 32-bit interrupt gate */
    idt_set_gate(128, (uint32_t) isr128, 0x08, 0xEE);
    register_interrupt_handler(128, &syscall_handler);
}

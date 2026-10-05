#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "process.h"
#include "fs.h"
#include "kheap.h"
#include "pipe.h"
#include "elf.h"
#include "usermode.h"
#include "vmm.h"
#include "serial.h"
#include "keyboard.h"
#include "bcache.h"

#define SYS_EXIT       0
#define SYS_WRITE      1
#define SYS_GETPID     2
#define SYS_OPEN       3
#define SYS_READ       4
#define SYS_CLOSE      5
#define SYS_SPAWN_WAIT 6
#define SYS_FORK       7
#define SYS_PIPE_WRITE 8
#define SYS_PIPE_READ  9
#define SYS_EXEC       10
#define SYS_WAIT       11
#define SYS_SBRK       12
#define SYS_SYNC       13

extern void isr128(void); /* defined in isr.s */

/* Spawns `name` as a new child of the calling process and reads its
   file in from disk -- the same steps shell.c's cmd_run performs, just
   triggered by a process instead of the shell. */
static process_t* spawn_child(const char* name, int parent_pid) {
    return process_spawn_by_name(name, parent_pid);
}

static void syscall_handler(struct registers* regs) {
    switch (regs->eax) {
        case SYS_WRITE: {
            int fd = (int) regs->ebx;
            const void* buf = (const void*) regs->ecx;
            uint32_t len = regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t)-1;
                break;
            }

            int n = vfs_write(me->fds[fd], buf, len);
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_EXIT:
            kprintf("[ok] process exited\n");
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
            const char* path = (const char*) regs->ebx;
            process_t* me = scheduler_current();
            int slot = -1;
            for (int i = 3; i < MAX_FDS; i++) {
                if (!me->fds[i]) { slot = i; break; }
            }
            if (slot < 0) { regs->eax = (uint32_t)-1; break; }

            vnode_t* vn = 0;
            if (vfs_resolve_path(path, me->cwd, &vn) != 0 || !vn) {
                regs->eax = (uint32_t)-1;
                break;
            }

            open_file_t* of = open_file_alloc(OPEN_FILE_VNODE, vn, O_RDONLY);
            if (!of) {
                vnode_unref(vn);
                regs->eax = (uint32_t)-1;
                break;
            }

            me->fds[slot] = of;
            regs->eax = (uint32_t) slot;
            break;
        }

        case SYS_READ: {
            int fd = (int) regs->ebx;
            void* buf = (void*) regs->ecx;
            uint32_t len = regs->edx;

            process_t* me = scheduler_current();
            if (fd < 0 || fd >= MAX_FDS || !me->fds[fd]) {
                regs->eax = (uint32_t)-1;
                break;
            }

            int n = vfs_read(me->fds[fd], buf, len);
            regs->eax = (uint32_t) n;
            break;
        }

        case SYS_CLOSE: {
            int fd = (int) regs->ebx;
            process_t* me = scheduler_current();
            if (fd >= 3 && fd < MAX_FDS && me->fds[fd]) {
                vfs_close(me->fds[fd]);
                me->fds[fd] = 0;
                regs->eax = 0;
            } else {
                regs->eax = (uint32_t)-1;
            }
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

        case SYS_FORK: {
            process_t* me = scheduler_current();
            process_t* child = process_fork(me, regs);
            /* Classic fork() semantics: the parent (still running this
               same syscall handler invocation) sees the child's pid.
               The child's OWN return value (0) was already baked into
               its saved_regs snapshot by process_fork() -- it'll see
               that the moment it's first scheduled in. */
            regs->eax = child ? (uint32_t) child->pid : (uint32_t)-1;
            break;
        }

        case SYS_PIPE_WRITE: {
            const uint8_t* buf = (const uint8_t*) regs->ebx;
            uint32_t len = regs->ecx;
            regs->eax = pipe_write(buf, len);
            break;
        }

        case SYS_PIPE_READ: {
            uint8_t* buf = (uint8_t*) regs->ebx;
            uint32_t maxlen = regs->ecx;
            /* pipe_read() may call scheduler_wait_for_pipe() internally,
               which blocks THIS process via the same switch_task
               mechanism as SYS_SPAWN_WAIT -- by the time it returns
               (possibly much later, after a writer provides data and
               this process gets rescheduled), regs may no longer be
               "live" in the usual sense, but writing to it here is
               still correct: it's the same trick every blocking
               syscall in this kernel uses. */
            regs->eax = pipe_read(buf, maxlen);
            break;
        }

        case SYS_EXEC: {
            const char* name = (const char*) regs->ebx;
            const char* const* uargv = (const char* const*) regs->ecx;

            process_t* me = scheduler_current();
            vnode_t* vn = 0;
            if (vfs_resolve_path(name, me->cwd, &vn) != 0 || !vn) {
                char bin_path[64];
                bin_path[0] = '/'; bin_path[1] = 'b'; bin_path[2] = 'i'; bin_path[3] = 'n'; bin_path[4] = '/';
                int l = 0;
                while (name[l] && l < 50) {
                    bin_path[5 + l] = name[l];
                    l++;
                }
                bin_path[5 + l] = '\0';
                if (vfs_resolve_path(bin_path, me->cwd, &vn) != 0 || !vn) {
                    regs->eax = (uint32_t)-1;
                    break;
                }
            }

            /* Copy arguments from user space before tearing down address space */
            int argc = 0;
            char kargv_buf[16][64];
            const char* kargv[17];
            if (uargv) {
                while (uargv[argc] && argc < 16) {
                    const char* uarg = uargv[argc];
                    int j = 0;
                    while (uarg[j] && j < 63) {
                        kargv_buf[argc][j] = uarg[j];
                        j++;
                    }
                    kargv_buf[argc][j] = '\0';
                    kargv[argc] = kargv_buf[argc];
                    argc++;
                }
            }
            if (argc == 0) {
                int j = 0;
                while (name[j] && j < 63) {
                    kargv_buf[0][j] = name[j];
                    j++;
                }
                kargv_buf[0][j] = '\0';
                kargv[0] = kargv_buf[0];
                argc = 1;
            }
            kargv[argc] = 0;

            uint32_t alloc_size = ((vn->size + 511) / 512) * 512;
            uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
            if (!buf) { vnode_unref(vn); regs->eax = (uint32_t)-1; break; }

            int n = vn->ops->read(vn, 0, buf, vn->size);
            vnode_unref(vn);
            if (n <= 0) { kfree(buf); regs->eax = (uint32_t)-1; break; }

            /* Build the NEW program's address space fully before
               touching the old one -- if anything fails, the calling
               process's current code/data is untouched and exec()
               correctly just returns -1 (real exec() semantics: it
               only fails to return if it succeeds). */
            address_space_t new_as = vmm_create_address_space();
            if (!new_as.directory) { kfree(buf); regs->eax = (uint32_t)-1; break; }

            uint32_t entry, stack_top;
            if (elf_load_into(buf, (uint32_t) n, &new_as, argc, kargv, &entry, &stack_top) != 0) {
                kfree(buf);
                vmm_destroy_address_space(&new_as);
                regs->eax = (uint32_t)-1;
                break;
            }
            kfree(buf);

            me = scheduler_current();
            vmm_destroy_address_space(&me->as); /* old program's memory reclaimed here */
            me->as = new_as;
            me->entry_point = entry;
            me->user_stack_top = stack_top;
            me->heap_end = HEAP_BASE;          /* the old heap died with the old address space */
            me->heap_mapped_up_to = HEAP_BASE;

            int i = 0; for (; name[i] && i < 31; i++) me->name[i] = name[i]; me->name[i] = '\0';

            /* This process keeps its pid, ppid, kernel stack, and open
               fds (real exec() semantics) -- only its address space and
               entry point changed. From here we jump straight into the
               new program; there is no "old regs" to return through,
               since the code that would have resumed there doesn't
               exist anymore. */
            vmm_switch(&me->as);
            enter_usermode(entry, stack_top); /* never returns */
            break; /* unreachable */
        }

        case SYS_WAIT: {
            int pid = (int) regs->ebx;
            process_t* target = process_find_by_pid(pid);
            if (!target) { regs->eax = 0; break; } /* already exited (or never existed) -- don't block forever */

            regs->eax = 0;
            scheduler_wait_for(pid); /* blocks; returns once pid exits */
            break;
        }

        case SYS_SBRK: {
            /* ebx holds a SIGNED increment (bytes to grow the heap by;
               0 just queries the current break; negative shrinks it,
               though we only move the bookkeeping back -- the pages
               stay mapped, a documented simplification vs a real brk()
               which could unmap them). Returns the PREVIOUS break,
               classic sbrk() semantics: the newly available range is
               [return value, return value + increment). */
            int32_t increment = (int32_t) regs->ebx;
            process_t* me = scheduler_current();

            uint32_t old_break = me->heap_end;
            uint32_t new_break = old_break + increment;
            int failed = 0;

            if (increment > 0) {
                if (new_break > HEAP_MAX || new_break < old_break /* overflow */) {
                    failed = 1;
                } else {
                    uint32_t target = (new_break + 4095u) & ~4095u;
                    while (!failed && me->heap_mapped_up_to < target) {
                        uint32_t frame_phys = vmm_map_user_page(&me->as, me->heap_mapped_up_to);
                        if (!frame_phys) { failed = 1; break; }
                        uint8_t* frame = (uint8_t*) frame_phys;
                        for (int i = 0; i < 4096; i++) frame[i] = 0; /* zero-initialized, matching real brk()/mmap() */
                        me->heap_mapped_up_to += 4096;
                    }
                }
            } else if (new_break < HEAP_BASE) {
                new_break = HEAP_BASE;
            }

            if (failed) { regs->eax = (uint32_t)-1; break; }

            me->heap_end = new_break;
            regs->eax = old_break;
            break;
        }

        case SYS_SYNC: {
            regs->eax = (uint32_t) bcache_sync();
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

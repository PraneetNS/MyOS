#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "ata.h"
#include "fs.h"
#include "process.h"
#include "scheduler.h"
#include "pipe.h"

#include "serial.h"
#include "multiboot2.h"
#include "bcache.h"
#include "vfs.h"
#include "fat16.h"
#include "tty.h"
#include "devfs.h"
#include "procfs.h"
#include "rtc.h"
#include "fpu.h"

static int check_boot_flag_kshell(uint32_t mb_info_addr) {
    if (!mb_info_addr) return 0;
    uint8_t* ptr = (uint8_t*)(uintptr_t) mb_info_addr;
    uint32_t total_size = *(uint32_t*) ptr;
    uint8_t* end = ptr + total_size;
    uint8_t* tag_ptr = ptr + 8;

    while (tag_ptr < end) {
        struct mb2_tag* tag = (struct mb2_tag*) tag_ptr;
        if (tag->type == MB2_TAG_TYPE_END) break;

        if (tag->type == MB2_TAG_TYPE_CMDLINE) {
            struct mb2_tag_string* cmd = (struct mb2_tag_string*) tag;
            const char* s = cmd->string;
            const char* needle = "kshell";
            for (int i = 0; s[i]; i++) {
                int j = 0;
                while (needle[j] && s[i + j] == needle[j]) j++;
                if (!needle[j]) return 1;
            }
        }
        tag_ptr += (tag->size + 7) & ~7u;
    }
    return 0;
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {
    (void) magic;

    serial_init();
    terminal_initialize();
    kprintf("MyOS kernel booted successfully.\n");

    gdt_install();
    kprintf("[ok] GDT installed\n");

    idt_install();
    kprintf("[ok] IDT installed, PIC remapped\n");

    timer_install(100);
    kprintf("[ok] PIT timer installed (100Hz)\n");

    rtc_init();
    kprintf("[ok] RTC driver initialized\n");

    keyboard_install();
    kprintf("[ok] Keyboard driver installed\n");

    serial_install_irq();
    kprintf("[ok] Serial COM1 IRQ installed\n");

    fpu_init();

    pmm_init(mb_info_addr);
    paging_init();
    kheap_init();

    asm volatile ("sti");
    kprintf("[ok] Interrupts enabled\n");

    kprintf("Free physical frames: %u\n", pmm_free_frame_count());

    tss_install(5, 0x10, 0);
    kprintf("[ok] TSS installed\n");
    /* TSS.esp0 is updated per-process by the scheduler on every switch
       (see scheduler.c's enter_process()) -- no one-shot setup needed
       here anymore, unlike Stage 5/6. */

    syscall_install();
    kprintf("[ok] Syscall interface installed (int 0x80)\n");

    bcache_init();
    kprintf("[ok] Buffer cache initialized\n");

    vfs_init();
    kprintf("[ok] VFS initialized\n");

    if (fat16_init() == 0) {
        vfs_mount("/", fat16_get_root());
        kprintf("[ok] Mounted FAT16 filesystem at /\n");
    } else {
        kprintf("[err] FAT16 init failed, falling back to MyFS\n");
        fs_init();
        vfs_mount("/", myfs_get_root_vnode());
    }

    tty_init();
    kprintf("[ok] TTY layer initialized\n");

    devfs_init();
    vfs_mount("/dev", devfs_get_root());
    devfs_get_root()->parent = vfs_get_root();
    vnode_ref(vfs_get_root());
    kprintf("[ok] Mounted devfs at /dev\n");

    procfs_init();
    vfs_mount("/proc", procfs_get_root());
    procfs_get_root()->parent = vfs_get_root();
    vnode_ref(vfs_get_root());
    kprintf("[ok] Mounted procfs at /proc\n");

    pipe_init();
    kprintf("[ok] Pipe (IPC) initialized\n");

    int use_kshell = check_boot_flag_kshell(mb_info_addr);
    process_init_table(use_kshell);
    if (use_kshell) {
        kprintf("[ok] Process table initialized (kshell = process 0)\n");
    } else {
        kprintf("[ok] Process table initialized (idle = process 0)\n");
        process_t* sh = process_spawn_by_name("sh.elf", 0);
        if (sh) {
            kprintf("[ok] Spawned sh.elf (PID %d)\n", sh->pid);
        } else {
            kprintf("[err] Failed to spawn sh.elf\n");
        }
    }

    kprintf("BOOT OK\n");

    scheduler_start(); /* never returns -- enters ring 3 sh.elf (or kshell if flag set) */

    for (;;) { asm volatile ("hlt"); }
}

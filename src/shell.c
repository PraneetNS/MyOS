#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "process.h"
#include "kheap.h"

static int streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Splits "cmd arg" in place at the first space; returns arg (or "" if none). */
static char* split_arg(char* line) {
    char* p = line;
    while (*p && *p != ' ') p++;
    if (*p == ' ') {
        *p = '\0';
        return p + 1;
    }
    return p; /* points at the trailing '\0' -- empty string */
}

static void cmd_help(void) {
    terminal_writestring(
        "Commands:\n"
        "  ls              list files on disk\n"
        "  cat <file>      print a text file's contents\n"
        "  run <file>      load an ELF binary and execute it in ring 3\n"
        "  help            show this message\n"
    );
}

static void cmd_cat(const char* name) {
    if (!*name) { terminal_writestring("usage: cat <file>\n"); return; }

    const fs_entry_t* e = fs_find(name);
    if (!e) { terminal_writestring("cat: no such file: "); terminal_writestring(name); terminal_writestring("\n"); return; }

    uint32_t alloc_size = ((e->size_bytes + 511) / 512) * 512 + 1;
    uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
    if (!buf) { terminal_writestring("cat: out of memory\n"); return; }

    int n = fs_read_file(e, buf);
    if (n < 0) { terminal_writestring("cat: read error\n"); kfree(buf); return; }

    buf[n] = '\0';
    terminal_writestring((const char*) buf);
    terminal_writestring("\n");
    kfree(buf);
}

static void cmd_run(const char* name) {
    if (!*name) { terminal_writestring("usage: run <file>\n"); return; }

    const fs_entry_t* e = fs_find(name);
    if (!e) { terminal_writestring("run: no such file: "); terminal_writestring(name); terminal_writestring("\n"); return; }

    uint32_t alloc_size = ((e->size_bytes + 511) / 512) * 512;
    uint8_t* buf = (uint8_t*) kmalloc(alloc_size);
    if (!buf) { terminal_writestring("run: out of memory\n"); return; }

    int n = fs_read_file(e, buf);
    if (n < 0) { terminal_writestring("run: read error\n"); kfree(buf); return; }

    /* process_spawn_from_elf() copies everything it needs into the new
       process's own frames synchronously, so -- unlike Stage 5/6, where
       enter_usermode() never returned and buf couldn't be freed until
       long after -- we can free buf immediately regardless of outcome. */
    process_t* p = process_spawn_from_elf(name, buf, (uint32_t) n, 0 /* parent = shell */);
    kfree(buf);

    if (p) {
        terminal_writestring("[shell] launched '");
        terminal_writestring(name);
        terminal_writestring("' as a new process -- it now runs concurrently,\n");
        terminal_writestring("        preemptively time-sliced against this shell.\n");
    }
}

void shell_run(void) {
    terminal_writestring("\nMyOS shell. Type 'help' for commands.\n");

    for (;;) {
        terminal_writestring("myos> ");

        char line[128];
        keyboard_read_line(line, sizeof(line));

        char* arg = split_arg(line);
        const char* cmd = line;

        if (streq(cmd, "")) continue;
        else if (streq(cmd, "help")) cmd_help();
        else if (streq(cmd, "ls"))   fs_list();
        else if (streq(cmd, "cat"))  cmd_cat(arg);
        else if (streq(cmd, "run"))  cmd_run(arg);
        else {
            terminal_writestring("Unknown command: ");
            terminal_writestring(cmd);
            terminal_writestring(" (try 'help')\n");
        }
    }
}

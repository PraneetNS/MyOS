#include "libc.h"

static void get_field(const char* buf, const char* key, char* out, int out_max) {
    int key_len = strlen(key);
    for (int i = 0; buf[i]; i++) {
        int match = 1;
        for (int k = 0; k < key_len; k++) {
            if (buf[i + k] != key[k]) { match = 0; break; }
        }
        if (match) {
            int p = i + key_len;
            while (buf[p] == ':' || buf[p] == '\t' || buf[p] == ' ') p++;
            int o = 0;
            while (buf[p] && buf[p] != '\n' && o < out_max - 1) {
                out[o++] = buf[p++];
            }
            out[o] = '\0';
            return;
        }
    }
    out[0] = '\0';
}

static void print_pad(const char* s, int width) {
    int len = strlen(s);
    write(1, s, len);
    for (int i = len; i < width; i++) {
        write(1, " ", 1);
    }
}

int main(void) {
    int proc_fd = open("/proc", O_RDONLY, 0);
    if (proc_fd < 0) {
        const char* err = "ps: cannot open /proc\n";
        write(2, err, strlen(err));
        return 1;
    }

    const char* hdr = "  PID  PPID  PGID  STAT  TICKS  VMPAGES  FDS  COMMAND\n";
    write(1, hdr, strlen(hdr));

    struct dirent de;
    while (getdents(proc_fd, &de, sizeof(de)) > 0) {
        if (de.d_name[0] < '0' || de.d_name[0] > '9') continue;

        char path[64];
        int p = 0;
        const char* prefix = "/proc/";
        while (prefix[p]) { path[p] = prefix[p]; p++; }
        int d = 0;
        while (de.d_name[d]) { path[p++] = de.d_name[d++]; }
        const char* suffix = "/status";
        int s = 0;
        while (suffix[s]) { path[p++] = suffix[s++]; }
        path[p] = '\0';

        int sfd = open(path, O_RDONLY, 0);
        if (sfd < 0) continue;

        char buf[512];
        int n = read(sfd, buf, sizeof(buf) - 1);
        close(sfd);
        if (n <= 0) continue;
        buf[n] = '\0';

        char pid_str[16], ppid_str[16], pgid_str[16], state_str[32], ticks_str[16], vm_str[16], fds_str[16], name_str[32];
        get_field(buf, "Pid", pid_str, sizeof(pid_str));
        get_field(buf, "PPid", ppid_str, sizeof(ppid_str));
        get_field(buf, "Pgid", pgid_str, sizeof(pgid_str));
        get_field(buf, "State", state_str, sizeof(state_str));
        get_field(buf, "Ticks", ticks_str, sizeof(ticks_str));
        get_field(buf, "VmPages", vm_str, sizeof(vm_str));
        get_field(buf, "FDSize", fds_str, sizeof(fds_str));
        get_field(buf, "Name", name_str, sizeof(name_str));

        /* Keep state character compact (e.g. 'R' from 'R (ready)') */
        char stat_ch[2] = { state_str[0] ? state_str[0] : '?', '\0' };

        write(1, "  ", 2);
        print_pad(pid_str, 5);
        write(1, " ", 1);
        print_pad(ppid_str, 5);
        write(1, " ", 1);
        print_pad(pgid_str, 5);
        write(1, " ", 1);
        print_pad(stat_ch, 5);
        write(1, " ", 1);
        print_pad(ticks_str, 6);
        write(1, " ", 1);
        print_pad(vm_str, 8);
        write(1, " ", 1);
        print_pad(fds_str, 4);
        write(1, " ", 1);
        write(1, name_str, strlen(name_str));
        write(1, "\n", 1);
    }

    close(proc_fd);
    return 0;
}

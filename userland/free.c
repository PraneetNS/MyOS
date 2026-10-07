#include "libc.h"

static unsigned int find_num_after(const char* buf, const char* key) {
    int key_len = strlen(key);
    for (int i = 0; buf[i]; i++) {
        int match = 1;
        for (int k = 0; k < key_len; k++) {
            if (buf[i + k] != key[k]) { match = 0; break; }
        }
        if (match) {
            int p = i + key_len;
            while (buf[p] == ' ' || buf[p] == '\t' || buf[p] == ':') p++;
            unsigned int v = 0;
            while (buf[p] >= '0' && buf[p] <= '9') {
                v = v * 10 + (buf[p] - '0');
                p++;
            }
            return v;
        }
    }
    return 0;
}

int main(void) {
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) {
        const char* err = "free: cannot open /proc/meminfo\n";
        write(2, err, strlen(err));
        return 1;
    }
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';

    unsigned int total_kb = find_num_after(buf, "MemTotal");
    unsigned int free_kb  = find_num_after(buf, "MemFree");
    unsigned int used_kb  = (total_kb >= free_kb) ? (total_kb - free_kb) : 0;

    unsigned int total_frames = find_num_after(buf, "FramesTotal");
    unsigned int free_frames  = find_num_after(buf, "FramesFree");
    unsigned int used_frames  = (total_frames >= free_frames) ? (total_frames - free_frames) : 0;

    write(1, "              total        used        free\n", 44);
    write(1, "Mem:     ", 9);
    print_uint(total_kb);
    write(1, " kB    ", 7);
    print_uint(used_kb);
    write(1, " kB    ", 7);
    print_uint(free_kb);
    write(1, " kB\n", 4);

    write(1, "Frames:  ", 9);
    print_uint(total_frames);
    write(1, "        ", 8);
    print_uint(used_frames);
    write(1, "        ", 8);
    print_uint(free_frames);
    write(1, "\n", 1);

    return 0;
}

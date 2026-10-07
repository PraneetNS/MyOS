#include "libc.h"

static int strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return *a - *b;
}

static int is_leap_year(int y) {
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static const char* const months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const int days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void print_2d(int v) {
    char b[2];
    b[0] = (char)('0' + (v / 10));
    b[1] = (char)('0' + (v % 10));
    write(1, b, 2);
}

static void print_sp_2d(int v) {
    char b[2];
    if (v < 10) {
        b[0] = ' ';
        b[1] = (char)('0' + v);
    } else {
        b[0] = (char)('0' + (v / 10));
        b[1] = (char)('0' + (v % 10));
    }
    write(1, b, 2);
}

static void print_time(unsigned int epoch, int tz_offset) {
    uint32_t target = (tz_offset >= 0) ? (epoch + (uint32_t)tz_offset * 60u) : (epoch - (uint32_t)(-tz_offset) * 60u);
    uint32_t rem = target;
    rem /= 60; /* minutes */
    int min = (int)(rem % 60); rem /= 60;
    int hour = (int)(rem % 24); rem /= 24;

    int year = 1970;
    for (;;) {
        int dim = is_leap_year(year) ? 366 : 365;
        if (rem < (uint32_t)dim) break;
        rem -= (uint32_t)dim;
        year++;
    }

    int leap = is_leap_year(year);
    int mon = 0;
    for (; mon < 12; mon++) {
        int dim = days_in_month[mon];
        if (mon == 1 && leap) dim = 29;
        if (rem < (uint32_t)dim) break;
        rem -= (uint32_t)dim;
    }
    int mday = (int)(rem + 1);

    write(1, months[mon], 3);
    write(1, " ", 1);
    print_sp_2d(mday);
    write(1, " ", 1);
    print_2d(hour);
    write(1, ":", 1);
    print_2d(min);
    write(1, " ", 1);
}

int main(int argc, char** argv) {
    int long_mode = 0;
    const char* path = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_mode = 1;
        } else {
            path = argv[i];
        }
    }

    int tz_offset = 0;
    if (long_mode) {
        int tz_fd = open("/etc/timezone", O_RDONLY, 0);
        if (tz_fd >= 0) {
            char buf[32];
            int n = read(tz_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                int sign = 1, idx = 0;
                while (buf[idx] == ' ' || buf[idx] == '\t') idx++;
                if (buf[idx] == '-') { sign = -1; idx++; }
                else if (buf[idx] == '+') { idx++; }
                int val = 0;
                while (buf[idx] >= '0' && buf[idx] <= '9') {
                    val = val * 10 + (buf[idx] - '0');
                    idx++;
                }
                tz_offset = sign * val;
            }
            close(tz_fd);
        }
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        const char* err1 = "ls: cannot access '";
        const char* err2 = "': No such file or directory\n";
        write(2, err1, strlen(err1));
        write(2, path, strlen(path));
        write(2, err2, strlen(err2));
        return 1;
    }

    struct dirent de;
    while (getdents(fd, &de, sizeof(de)) > 0) {
        if (de.d_name[0] == '\0') continue;

        if (long_mode) {
            if (de.d_type == 2) {
                write(1, "d ", 2);
            } else {
                write(1, "- ", 2);
            }
            print_uint(de.d_size);
            write(1, "\t", 1);

            char item_path[128];
            int p = 0;
            while (path[p] && p < 100) { item_path[p] = path[p]; p++; }
            if (p > 0 && item_path[p - 1] != '/') item_path[p++] = '/';
            int d = 0;
            while (de.d_name[d] && p < 126) { item_path[p++] = de.d_name[d++]; }
            item_path[p] = '\0';

            struct stat st;
            if (stat(item_path, &st) == 0 && st.st_mtime != 0) {
                print_time(st.st_mtime, tz_offset);
            }

            write(1, de.d_name, strlen(de.d_name));
            write(1, "\n", 1);
        } else {
            write(1, de.d_name, strlen(de.d_name));
            write(1, "\n", 1);
        }
    }

    close(fd);
    return 0;
}

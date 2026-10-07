#include "libc.h"

static int is_leap_year(int y) {
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static const char* const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
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

static void print_4d(int v) {
    char b[4];
    b[0] = (char)('0' + (v / 1000));
    b[1] = (char)('0' + ((v / 100) % 10));
    b[2] = (char)('0' + ((v / 10) % 10));
    b[3] = (char)('0' + (v % 10));
    write(1, b, 4);
}

int main(int argc, char** argv) {
    int utc_only = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'u') {
            utc_only = 1;
        }
    }

    int tz_offset = 0;
    if (!utc_only) {
        int fd = open("/etc/timezone", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[32];
            int n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                int sign = 1;
                int idx = 0;
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
            close(fd);
        }
    }

    time_t cur = time(0);
    uint32_t target_epoch = (tz_offset >= 0) ? (cur + (uint32_t)tz_offset * 60u) : (cur - (uint32_t)(-tz_offset) * 60u);
    uint32_t rem = target_epoch;
    int sec = (int)(rem % 60); rem /= 60;
    int min = (int)(rem % 60); rem /= 60;
    int hour = (int)(rem % 24); rem /= 24;

    int wday = (int)((rem + 4) % 7);
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

    /* Format: Wed Oct  7 22:34:37 UTC 2026 */
    write(1, wdays[wday], strlen(wdays[wday]));
    write(1, " ", 1);
    write(1, months[mon], strlen(months[mon]));
    write(1, " ", 1);
    print_sp_2d(mday);
    write(1, " ", 1);
    print_2d(hour);
    write(1, ":", 1);
    print_2d(min);
    write(1, ":", 1);
    print_2d(sec);
    write(1, " ", 1);
    if (utc_only || tz_offset == 0) {
        write(1, "UTC ", 4);
    } else if (tz_offset == 330) {
        write(1, "IST ", 4);
    } else {
        write(1, "LOC ", 4);
    }
    print_4d(year);
    write(1, "\n", 1);

    return 0;
}

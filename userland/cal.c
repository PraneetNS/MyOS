#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static const char* month_names[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int month, int year) {
    static const int days[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && is_leap_year(year)) return 29;
    return days[month];
}

/* Returns 0 = Sunday, 1 = Monday, ..., 6 = Saturday for 1st of month */
static int day_of_week_first(int month, int year) {
    /* Tomohiko Sakamoto's algorithm */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year - (month < 3);
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + 1) % 7;
}

static void print_month(int month, int year) {
    char header[32];
    snprintf(header, sizeof(header), "%s %d", month_names[month], year);
    int pad = (20 - (int)strlen(header)) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; i++) putchar(' ');
    printf("%s\n", header);
    printf("Su Mo Tu We Th Fr Sa\n");

    int start_dow = day_of_week_first(month, year);
    int total_days = days_in_month(month, year);

    for (int i = 0; i < start_dow; i++) {
        printf("   ");
    }

    int cur_dow = start_dow;
    for (int day = 1; day <= total_days; day++) {
        printf("%2d", day);
        cur_dow++;
        if (cur_dow == 7 || day == total_days) {
            putchar('\n');
            cur_dow = 0;
        } else {
            putchar(' ');
        }
    }
}

int main(int argc, char* argv[]) {
    int month = 0;
    int year = 0;

    if (argc == 1) {
        time_t now = time(NULL);
        struct tm* t = localtime(&now);
        if (t) {
            month = t->tm_mon + 1;
            year = t->tm_year + 1900;
        } else {
            month = 10;
            year = 2026;
        }
    } else if (argc == 2) {
        year = atoi(argv[1]);
        if (year < 1 || year > 9999) {
            fprintf(stderr, "cal: invalid year %s\n", argv[1]);
            return 1;
        }
        for (int m = 1; m <= 12; m++) {
            print_month(m, year);
            if (m < 12) putchar('\n');
        }
        return 0;
    } else if (argc == 3) {
        month = atoi(argv[1]);
        year = atoi(argv[2]);
        if (month < 1 || month > 12) {
            fprintf(stderr, "cal: invalid month %s (1-12)\n", argv[1]);
            return 1;
        }
        if (year < 1 || year > 9999) {
            fprintf(stderr, "cal: invalid year %s\n", argv[2]);
            return 1;
        }
    } else {
        fprintf(stderr, "Usage: cal [month] [year]\n");
        return 1;
    }

    print_month(month, year);
    return 0;
}

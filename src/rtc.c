#include "rtc.h"
#include "io.h"
#include "timer.h"
#include "vga.h"
#include "serial.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint32_t boot_epoch = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int cmos_is_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80);
}

static int is_leap_year(int y) {
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static const int days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static const int days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

uint32_t rtc_time_to_epoch(int year, int month, int day, int hour, int min, int sec) {
    if (year < 1970) year = 1970;
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (day < 1) day = 1;
    if (day > 31) day = 31;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    if (min < 0) min = 0;
    if (min > 59) min = 59;
    if (sec < 0) sec = 0;
    if (sec > 59) sec = 59;

    uint32_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    days += (uint32_t)days_before_month[month - 1];
    if (month > 2 && is_leap_year(year)) {
        days += 1;
    }
    days += (uint32_t)(day - 1);

    return days * 86400u + (uint32_t)hour * 3600u + (uint32_t)min * 60u + (uint32_t)sec;
}

void rtc_epoch_to_time(uint32_t epoch, rtc_time_t* tm) {
    if (!tm) return;
    uint32_t rem = epoch;
    tm->sec = (int)(rem % 60); rem /= 60;
    tm->min = (int)(rem % 60); rem /= 60;
    tm->hour = (int)(rem % 24); rem /= 24;

    /* 1970-01-01 was Thursday (4) */
    tm->wday = (int)((rem + 4) % 7);

    int y = 1970;
    for (;;) {
        int dim = is_leap_year(y) ? 366 : 365;
        if (rem < (uint32_t)dim) break;
        rem -= (uint32_t)dim;
        y++;
    }
    tm->year = y;

    int leap = is_leap_year(y);
    int m = 0;
    for (; m < 12; m++) {
        int dim = days_in_month[m];
        if (m == 1 && leap) dim = 29;
        if (rem < (uint32_t)dim) break;
        rem -= (uint32_t)dim;
    }
    tm->month = m + 1;
    tm->day = (int)(rem + 1);
}

#define BCD_TO_BIN(val) ((((val) >> 4) * 10) + ((val) & 0x0F))

static void read_raw_cmos(uint8_t* sec, uint8_t* min, uint8_t* hr,
                          uint8_t* day, uint8_t* mo, uint8_t* yr, uint8_t* cent) {
    *sec = cmos_read(0x00);
    *min = cmos_read(0x02);
    *hr  = cmos_read(0x04);
    *day = cmos_read(0x07);
    *mo  = cmos_read(0x08);
    *yr  = cmos_read(0x09);
    *cent = cmos_read(0x32);
}

void rtc_init(void) {
    /* Wait until update-in-progress flag is clear */
    int timeout = 100000;
    while (cmos_is_updating() && --timeout > 0);

    uint8_t sec1, min1, hr1, day1, mo1, yr1, cent1;
    uint8_t sec2, min2, hr2, day2, mo2, yr2, cent2;

    /* Read twice until two consecutive reads match */
    for (;;) {
        timeout = 100000;
        while (cmos_is_updating() && --timeout > 0);
        read_raw_cmos(&sec1, &min1, &hr1, &day1, &mo1, &yr1, &cent1);

        timeout = 100000;
        while (cmos_is_updating() && --timeout > 0);
        read_raw_cmos(&sec2, &min2, &hr2, &day2, &mo2, &yr2, &cent2);

        if (sec1 == sec2 && min1 == min2 && hr1 == hr2 &&
            day1 == day2 && mo1 == mo2 && yr1 == yr2 && cent1 == cent2) {
            break;
        }
    }

    uint8_t status_b = cmos_read(0x0B);
    int is_binary = (status_b & 0x04) != 0;
    int is_24hr   = (status_b & 0x02) != 0;

    int sec = sec1;
    int min = min1;
    int hr  = hr1;
    int day = day1;
    int mo  = mo1;
    int yr  = yr1;
    int cent = cent1;

    /* Handle 12-hour vs 24-hour mode */
    if (!is_24hr) {
        int pm = (hr & 0x80) != 0;
        hr &= 0x7F;
        if (!is_binary) hr = BCD_TO_BIN(hr);
        if (pm && hr < 12) hr += 12;
        if (!pm && hr == 12) hr = 0;
    } else if (!is_binary) {
        hr = BCD_TO_BIN(hr);
    }

    if (!is_binary) {
        sec = BCD_TO_BIN(sec);
        min = BCD_TO_BIN(min);
        day = BCD_TO_BIN(day);
        mo  = BCD_TO_BIN(mo);
        yr  = BCD_TO_BIN(yr);
        if (cent != 0) cent = BCD_TO_BIN(cent);
    }

    /* Century handling with fallback to 20xx */
    int full_year;
    if (cent >= 19 && cent <= 21) {
        full_year = cent * 100 + yr;
    } else {
        full_year = 2000 + yr;
    }

    boot_epoch = rtc_time_to_epoch(full_year, mo, day, hr, min, sec);

    rtc_time_t tm;
    rtc_epoch_to_time(boot_epoch, &tm);

    kprintf("[rtc] RTC boot time: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %u)\n",
            tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec, boot_epoch);
    serial_printf("[rtc] RTC boot time: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %u)\n",
                  tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec, boot_epoch);
}

uint32_t rtc_get_boot_epoch(void) {
    return boot_epoch;
}

uint32_t rtc_get_epoch(void) {
    return boot_epoch + (timer_get_ticks() / 100u);
}

uint32_t fat_datetime_to_epoch(uint16_t date, uint16_t time) {
    if (date == 0) return 0;
    int day   = date & 0x1F;
    int month = (date >> 5) & 0x0F;
    int year  = 1980 + ((date >> 9) & 0x7F);
    int sec   = (time & 0x1F) * 2;
    int min   = (time >> 5) & 0x3F;
    int hour  = (time >> 11) & 0x1F;
    return rtc_time_to_epoch(year, month, day, hour, min, sec);
}

void epoch_to_fat_datetime(uint32_t epoch, uint16_t* date, uint16_t* time) {
    if (!date || !time) return;
    if (epoch == 0) {
        *date = 0;
        *time = 0;
        return;
    }
    rtc_time_t tm;
    rtc_epoch_to_time(epoch, &tm);
    int yr_off = tm.year >= 1980 ? tm.year - 1980 : 0;
    if (yr_off > 127) yr_off = 127;
    *date = (uint16_t)(((yr_off & 0x7F) << 9) | ((tm.month & 0x0F) << 5) | (tm.day & 0x1F));
    *time = (uint16_t)(((tm.hour & 0x1F) << 11) | ((tm.min & 0x3F) << 5) | ((tm.sec / 2) & 0x1F));
}

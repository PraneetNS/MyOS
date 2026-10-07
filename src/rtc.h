#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    int year;   /* e.g. 2026 */
    int month;  /* 1-12 */
    int day;    /* 1-31 */
    int hour;   /* 0-23 */
    int min;    /* 0-59 */
    int sec;    /* 0-59 */
    int wday;   /* 0=Sun, 1=Mon, ..., 6=Sat */
} rtc_time_t;

/* Initialize RTC driver, read CMOS time and log boot time */
void rtc_init(void);

/* Get system boot epoch in seconds */
uint32_t rtc_get_boot_epoch(void);

/* Get current wall clock epoch in seconds (UTC) */
uint32_t rtc_get_epoch(void);

/* Convert broken-down UTC time to Unix epoch */
uint32_t rtc_time_to_epoch(int year, int month, int day, int hour, int min, int sec);

/* Convert Unix epoch to broken-down UTC time */
void rtc_epoch_to_time(uint32_t epoch, rtc_time_t* tm);

/* FAT16 timestamp helpers */
uint32_t fat_datetime_to_epoch(uint16_t date, uint16_t time);
void epoch_to_fat_datetime(uint32_t epoch, uint16_t* date, uint16_t* time);

#endif

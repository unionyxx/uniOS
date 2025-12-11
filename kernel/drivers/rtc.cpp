#include "rtc.h"
#include "io.h"
#include "timer.h"

// CMOS ports
#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

// CMOS registers
#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_WEEKDAY     0x06
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B

static uint64_t boot_ticks = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool rtc_update_in_progress() {
    outb(CMOS_ADDR, RTC_STATUS_A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void rtc_init() {
    boot_ticks = timer_get_ticks();
}

void rtc_get_time(RTCTime* time) {
    // Wait for update to complete
    while (rtc_update_in_progress());
    
    uint8_t second = cmos_read(RTC_SECONDS);
    uint8_t minute = cmos_read(RTC_MINUTES);
    uint8_t hour = cmos_read(RTC_HOURS);
    uint8_t day = cmos_read(RTC_DAY);
    uint8_t month = cmos_read(RTC_MONTH);
    uint8_t year = cmos_read(RTC_YEAR);
    uint8_t weekday = cmos_read(RTC_WEEKDAY);
    
    // Read again to ensure consistency
    while (rtc_update_in_progress());
    
    uint8_t second2 = cmos_read(RTC_SECONDS);
    uint8_t minute2 = cmos_read(RTC_MINUTES);
    uint8_t hour2 = cmos_read(RTC_HOURS);
    uint8_t day2 = cmos_read(RTC_DAY);
    uint8_t month2 = cmos_read(RTC_MONTH);
    uint8_t year2 = cmos_read(RTC_YEAR);
    
    // If values differ, read again
    if (second != second2 || minute != minute2 || hour != hour2 ||
        day != day2 || month != month2 || year != year2) {
        second = second2;
        minute = minute2;
        hour = hour2;
        day = day2;
        month = month2;
        year = year2;
    }
    
    // Check if BCD mode (default on most systems)
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    if (!(status_b & 0x04)) {
        // BCD mode - convert to binary
        second = bcd_to_binary(second);
        minute = bcd_to_binary(minute);
        hour = bcd_to_binary(hour & 0x7F) | (hour & 0x80);  // Preserve PM bit
        day = bcd_to_binary(day);
        month = bcd_to_binary(month);
        year = bcd_to_binary(year);
    }
    
    // Convert 12-hour to 24-hour if needed
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }
    
    time->second = second;
    time->minute = minute;
    time->hour = hour;
    time->day = day;
    time->month = month;
    time->year = 2000 + year;  // Assume 2000s
    time->weekday = weekday;
}

uint64_t rtc_get_uptime_seconds() {
    uint64_t current = timer_get_ticks();
    uint64_t elapsed = current - boot_ticks;
    // Timer is 100Hz, so 100 ticks = 1 second
    return elapsed / 100;
}

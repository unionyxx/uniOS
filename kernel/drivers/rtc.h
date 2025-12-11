#pragma once

#include <stdint.h>

struct RTCTime {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t weekday;
};

// Initialize RTC (not much needed, but for consistency)
void rtc_init();

// Read current time from CMOS RTC
void rtc_get_time(RTCTime* time);

// Get uptime in seconds
uint64_t rtc_get_uptime_seconds();

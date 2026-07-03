#pragma once
#include <Arduino.h>

// PCF8563 battery-backed RTC over the shared I2C bus (Wire.begin already
// called by the caller — same bus as the CHSC6X touch controller).

#define PCF8563_ADDR 0x51

// Probes the chip. Returns false if nothing acks at 0x51 (not wired / dead).
bool rtc_init();

// True once the chip has a time value it trusts (VL flag clear). False after
// a dead/missing coin cell or before the first rtc_set_time() call.
bool rtc_time_valid();

// Reads current time. Returns false if the chip is unreachable or VL is set.
bool rtc_get_time(int &h, int &m, int &s);

// Writes time and clears the VL flag. Call once after an NTP sync.
bool rtc_set_time(int h, int m, int s, int wday = 0, int mday = 1, int mon = 1, int year = 2026);

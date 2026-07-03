#include "rtc.h"
#include <Wire.h>

static inline uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }
static inline uint8_t bin2bcd(uint8_t v) { return (v % 10) | ((v / 10) << 4); }

bool rtc_init() {
    Wire.beginTransmission(PCF8563_ADDR);
    bool ok = (Wire.endTransmission() == 0);
    if (ok) {
        Serial.println("[RTC] init OK");
        return true;
    }

    // Seen on-device: the very first I2C transaction right after Wire.begin()
    // can NACK if the chip hasn't settled from a cold power-up yet. One quick
    // retry clears most of these (mirrors the same pattern in cam_init()).
    Serial.println("[RTC] init: no ack at 0x51, retrying once after 50ms...");
    delay(50);
    Wire.beginTransmission(PCF8563_ADDR);
    ok = (Wire.endTransmission() == 0);
    Serial.printf("[RTC] init %s\n", ok ? "OK (after retry)" : "FAILED (no ack at 0x51)");
    return ok;
}

bool rtc_time_valid() {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(PCF8563_ADDR, 1) != 1) return false;
    uint8_t sec_reg = Wire.read();
    return (sec_reg & 0x80) == 0;  // VL bit set = time integrity lost
}

bool rtc_get_time(int &h, int &m, int &s) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(PCF8563_ADDR, 3) != 3) return false;
    uint8_t sec_reg = Wire.read();
    uint8_t min_reg = Wire.read();
    uint8_t hr_reg  = Wire.read();
    if (sec_reg & 0x80) return false;  // VL flag — time unreliable
    s = bcd2bin(sec_reg & 0x7F);
    m = bcd2bin(min_reg & 0x7F);
    h = bcd2bin(hr_reg  & 0x3F);
    return true;
}

bool rtc_set_time(int h, int m, int s, int wday, int mday, int mon, int year) {
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(0x02);
    Wire.write(bin2bcd(s));   // writing seconds first clears the VL flag
    Wire.write(bin2bcd(m));
    Wire.write(bin2bcd(h));
    Wire.write(bin2bcd(mday));
    Wire.write(bin2bcd(wday));
    Wire.write(bin2bcd(mon));
    Wire.write(bin2bcd(year % 100));
    bool ok = (Wire.endTransmission() == 0);
    Serial.printf("[RTC] set %02d:%02d:%02d %s\n", h, m, s, ok ? "OK" : "FAILED");
    return ok;
}

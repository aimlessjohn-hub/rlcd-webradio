#include "pcf85063.h"
#include <Wire.h>

#define PCF85063_I2C_ADDR 0x51
#define PCF85063_CTRL1    0x00
#define PCF85063_SEC_REG  0x04

static bool rtc_found = false;

static inline uint8_t bcd2dec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static inline uint8_t dec2bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

bool pcf85063_init() {
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        rtc_found = false;
        Serial.println("[RTC] PCF85063 nicht auf I2C 0x51 gefunden.");
        return false;
    }
    rtc_found = true;

    // CTRL1: Clear STOP bit (bit 5) so the oscillator runs, 24-hour mode (bit 1 = 0)
    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(PCF85063_CTRL1);
    Wire.write(0x00);
    Wire.endTransmission();

    Serial.println("[RTC] PCF85063 Hardware-Uhr erfolgreich initialisiert.");
    return true;
}

bool pcf85063_is_available() {
    return rtc_found;
}

bool pcf85063_read_time(struct tm *ti) {
    if (!rtc_found) return false;

    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(PCF85063_SEC_REG);
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom((uint8_t)PCF85063_I2C_ADDR, (uint8_t)7) != 7) return false;

    uint8_t sec_raw = Wire.read();
    uint8_t min_raw = Wire.read();
    uint8_t hr_raw  = Wire.read();
    uint8_t day_raw = Wire.read();
    uint8_t wday_raw= Wire.read();
    uint8_t mon_raw = Wire.read();
    uint8_t yr_raw  = Wire.read();

    if (ti) {
        memset(ti, 0, sizeof(struct tm));
        ti->tm_sec  = bcd2dec(sec_raw & 0x7F);
        ti->tm_min  = bcd2dec(min_raw & 0x7F);
        ti->tm_hour = bcd2dec(hr_raw  & 0x3F);
        ti->tm_mday = bcd2dec(day_raw & 0x3F);
        ti->tm_mon  = bcd2dec(mon_raw & 0x1F) - 1;
        ti->tm_year = bcd2dec(yr_raw) + 100; // e.g. 26 -> 126 (Year 2026)
        ti->tm_wday = wday_raw & 0x07;
    }
    return true;
}

bool pcf85063_set_time(const struct tm *ti) {
    if (!rtc_found || !ti) return false;

    Wire.beginTransmission(PCF85063_I2C_ADDR);
    Wire.write(PCF85063_SEC_REG);
    Wire.write(dec2bcd(ti->tm_sec) & 0x7F);
    Wire.write(dec2bcd(ti->tm_min) & 0x7F);
    Wire.write(dec2bcd(ti->tm_hour) & 0x3F);
    Wire.write(dec2bcd(ti->tm_mday) & 0x3F);
    Wire.write(ti->tm_wday & 0x07);
    Wire.write(dec2bcd(ti->tm_mon + 1) & 0x1F);
    Wire.write(dec2bcd(ti->tm_year % 100));
    return (Wire.endTransmission() == 0);
}

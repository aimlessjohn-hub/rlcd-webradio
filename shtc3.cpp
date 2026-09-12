#include "shtc3.h"
#include <Wire.h>

#define SHTC3_I2C_ADDR 0x70

// SHTC3 Commands
#define SHTC3_CMD_WAKEUP           0x3517
#define SHTC3_CMD_SLEEP            0xB098
#define SHTC3_CMD_SOFT_RESET       0x805D
#define SHTC3_CMD_READ_ID          0xEFC8
#define SHTC3_CMD_MEASURE_NORMAL   0x7866 // Clock stretching disabled, T first

static uint8_t shtc3_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static bool shtc3_send_cmd(uint16_t cmd) {
    Wire.beginTransmission(SHTC3_I2C_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)(cmd & 0xFF));
    return (Wire.endTransmission() == 0);
}

bool shtc3_init() {
    // Wakeup
    if (!shtc3_send_cmd(SHTC3_CMD_WAKEUP)) {
        Serial.println("[SHTC3] Wakeup fehlgeschlagen");
        return false;
    }
    delay(2);

    // Soft reset
    shtc3_send_cmd(SHTC3_CMD_SOFT_RESET);
    delay(2);

    shtc3_send_cmd(SHTC3_CMD_WAKEUP);
    delay(2);

    // Read ID to verify sensor presence
    if (!shtc3_send_cmd(SHTC3_CMD_READ_ID)) {
        Serial.println("[SHTC3] Read ID fehlgeschlagen");
        return false;
    }
    delay(1);

    if (Wire.requestFrom((uint8_t)SHTC3_I2C_ADDR, (uint8_t)3) == 3) {
        uint8_t id_msb = Wire.read();
        uint8_t id_lsb = Wire.read();
        uint8_t id_crc = Wire.read();
        uint8_t data[2] = { id_msb, id_lsb };
        if (shtc3_crc8(data, 2) == id_crc) {
            uint16_t id = (id_msb << 8) | id_lsb;
            Serial.printf("[OK] SHTC3 Sensor erkannt! ID: 0x%04X\n", id);
            shtc3_send_cmd(SHTC3_CMD_SLEEP);
            return true;
        } else {
            Serial.println("[SHTC3] ID CRC Fehler!");
        }
    }

    shtc3_send_cmd(SHTC3_CMD_SLEEP);
    return false;
}

bool shtc3_read(float *temperature, float *humidity, float offset_c) {
    if (!temperature || !humidity) return false;

    // 1. Wakeup
    if (!shtc3_send_cmd(SHTC3_CMD_WAKEUP)) {
        return false;
    }
    delayMicroseconds(300);

    // 2. Start normal measurement (T first, clock stretching disabled)
    if (!shtc3_send_cmd(SHTC3_CMD_MEASURE_NORMAL)) {
        shtc3_send_cmd(SHTC3_CMD_SLEEP);
        return false;
    }

    // Normal measurement takes typ. 12.1ms for T + 3.7ms for RH
    delay(16);

    // 3. Read 6 bytes: T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC
    if (Wire.requestFrom((uint8_t)SHTC3_I2C_ADDR, (uint8_t)6) != 6) {
        shtc3_send_cmd(SHTC3_CMD_SLEEP);
        return false;
    }

    uint8_t t_msb = Wire.read();
    uint8_t t_lsb = Wire.read();
    uint8_t t_crc = Wire.read();
    uint8_t rh_msb = Wire.read();
    uint8_t rh_lsb = Wire.read();
    uint8_t rh_crc = Wire.read();

    // 4. Put sensor back to sleep
    shtc3_send_cmd(SHTC3_CMD_SLEEP);

    // 5. Verify CRCs
    uint8_t t_buf[2] = { t_msb, t_lsb };
    if (shtc3_crc8(t_buf, 2) != t_crc) {
        return false;
    }

    uint8_t rh_buf[2] = { rh_msb, rh_lsb };
    if (shtc3_crc8(rh_buf, 2) != rh_crc) {
        return false;
    }

    // 6. Calculate values
    uint16_t rawT = ((uint16_t)t_msb << 8) | t_lsb;
    uint16_t rawRH = ((uint16_t)rh_msb << 8) | rh_lsb;

    *temperature = -45.0f + 175.0f * (float)rawT / 65536.0f - offset_c;
    *humidity = 100.0f * (float)rawRH / 65536.0f;
    if (*humidity < 0.0f) *humidity = 0.0f;
    if (*humidity > 100.0f) *humidity = 100.0f;

    return true;
}

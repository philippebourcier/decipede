#include "sht4x.h"

#include <stdint.h>

#include "board.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#define CMD_MEASURE_HIGH 0xFD
#define I2C_TIMEOUT_US   20000

static uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

bool sht40_read(float *temperature_c, float *humidity_pct) {
    uint8_t cmd = CMD_MEASURE_HIGH;
    if (i2c_write_timeout_us(SENSOR_I2C, SHT40_ADDR, &cmd, 1, false, I2C_TIMEOUT_US) != 1) return false;
    sleep_ms(10);
    uint8_t d[6];
    if (i2c_read_timeout_us(SENSOR_I2C, SHT40_ADDR, d, 6, false, I2C_TIMEOUT_US) != 6) return false;
    if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return false;

    uint16_t t_raw = (uint16_t)(d[0] << 8 | d[1]);
    uint16_t h_raw = (uint16_t)(d[3] << 8 | d[4]);
    float h = -6.0f + 125.0f * (h_raw / 65535.0f);
    if (h < 0.0f) h = 0.0f;
    if (h > 100.0f) h = 100.0f;
    *temperature_c = -45.0f + 175.0f * (t_raw / 65535.0f);
    *humidity_pct = h;
    return true;
}

#include "lsm6dsv.h"

#include <math.h>
#include <stdio.h>

#include "board.h"
#include "hardware/i2c.h"
#include "idle.h"
#include "pico/time.h"

#define REG_WHO_AM_I   0x0F
#define REG_CTRL1      0x10
#define REG_CTRL2      0x11
#define REG_CTRL3      0x12
#define REG_CTRL6      0x15
#define REG_CTRL8      0x17
#define REG_STATUS     0x1E
#define REG_OUTX_L_G   0x22
#define REG_OUTX_L_A   0x28

#define DEVICE_ID      0x70
#define CTRL3_BDU      (1 << 6)
#define CTRL3_IF_INC   (1 << 2)
#define CTRL3_SW_RESET 0x01
#define ODR_30HZ       0x04
#define ODR_120HZ      0x06
#define FS_XL_2G       0x00
#define FS_GY_250      0x01
#define SENS_XL_MG     0.061f   // mg / LSB
#define SENS_GY_MDPS   8.750f   // mdps / LSB

#define STATUS_XLDA    0x01
#define STATUS_GDA     0x02
#define TILT_SAMPLES   32
#define I2C_TIMEOUT_US 20000

static bool read_regs(lsm6dsv_t *imu, uint8_t reg, uint8_t *buf, size_t n) {
    if (i2c_write_timeout_us(SENSOR_I2C, imu->addr, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(SENSOR_I2C, imu->addr, buf, n, false, I2C_TIMEOUT_US) == (int)n;
}

static bool write_reg(lsm6dsv_t *imu, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(SENSOR_I2C, imu->addr, buf, 2, false, I2C_TIMEOUT_US) == 2;
}

static bool wait_drdy(lsm6dsv_t *imu, uint8_t mask) {
    absolute_time_t deadline = make_timeout_time_ms(100);
    uint8_t st;
    for (;;) {
        if (!read_regs(imu, REG_STATUS, &st, 1)) return false;
        if (st & mask) return true;
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return false;
        idle_poll();
        // Samples come every 8 ms (gyro) or 33 ms (accel) and stay latched
        // until read, so sleep instead of hammering the I2C bus.
        sleep_ms(1);
    }
}

static bool read_xyz(lsm6dsv_t *imu, uint8_t reg, int16_t v[3]) {
    uint8_t d[6];
    if (!read_regs(imu, reg, d, 6)) return false;
    for (int i = 0; i < 3; i++) v[i] = (int16_t)(d[2 * i + 1] << 8 | d[2 * i]);
    return true;
}

bool lsm6dsv_init(lsm6dsv_t *imu) {
    uint8_t who = 0;
    if (!read_regs(imu, REG_WHO_AM_I, &who, 1)) {
        printf("LSM6DSV16X: no response at 0x%02X\n", imu->addr);
        return false;
    }
    if (who != DEVICE_ID) {
        printf("LSM6DSV16X not found (WHO_AM_I=0x%02X, expected 0x%02X)\n", who, DEVICE_ID);
        return false;
    }
    if (!write_reg(imu, REG_CTRL3, CTRL3_SW_RESET)) return false;
    sleep_ms(15);
    return write_reg(imu, REG_CTRL3, CTRL3_BDU | CTRL3_IF_INC) &&
           write_reg(imu, REG_CTRL8, FS_XL_2G) &&
           write_reg(imu, REG_CTRL6, FS_GY_250) &&
           write_reg(imu, REG_CTRL1, ODR_30HZ) &&
           write_reg(imu, REG_CTRL2, ODR_120HZ);
}

static bool vibration_rms(lsm6dsv_t *imu, float *rms_max) {
    double sq[3] = {0, 0, 0};
    int n = 0;
    absolute_time_t t_end = make_timeout_time_ms(imu->vibration_window_ms);
    while (absolute_time_diff_us(get_absolute_time(), t_end) > 0) {
        int16_t g[3];
        if (!wait_drdy(imu, STATUS_GDA) || !read_xyz(imu, REG_OUTX_L_G, g)) return false;
        for (int i = 0; i < 3; i++) {
            double dps = g[i] * SENS_GY_MDPS / 1000.0;
            sq[i] += dps * dps;
        }
        n++;
    }
    float m = 0.0f;
    if (n) {
        for (int i = 0; i < 3; i++) {
            float r = (float)sqrt(sq[i] / n);
            if (r > m) m = r;
        }
    }
    *rms_max = m;
    return true;
}

static bool tilt(lsm6dsv_t *imu, float *pitch, float *roll) {
    int32_t s[3] = {0, 0, 0};
    for (int k = 0; k < TILT_SAMPLES; k++) {
        int16_t a[3];
        if (!wait_drdy(imu, STATUS_XLDA) || !read_xyz(imu, REG_OUTX_L_A, a)) return false;
        for (int i = 0; i < 3; i++) s[i] += a[i];
    }
    double ax = (double)s[0] / TILT_SAMPLES * SENS_XL_MG;
    double ay = (double)s[1] / TILT_SAMPLES * SENS_XL_MG;
    double az = (double)s[2] / TILT_SAMPLES * SENS_XL_MG;
    *pitch = (float)(atan2(ax, sqrt(ay * ay + az * az)) * 180.0 / M_PI);
    *roll = (float)(atan2(ay, sqrt(ax * ax + az * az)) * 180.0 / M_PI);
    return true;
}

bool lsm6dsv_check(lsm6dsv_t *imu, imu_check_t *out) {
    // Vibration first: tilt from a shaking accelerometer is meaningless.
    if (!vibration_rms(imu, &out->rms_max_dps)) return false;
    out->vibrating = out->rms_max_dps > imu->vibration_threshold_dps;
    out->level_valid = false;
    out->level = false;
    out->pitch_deg = out->roll_deg = 0.0f;
    if (!out->vibrating) {
        if (!tilt(imu, &out->pitch_deg, &out->roll_deg)) return false;
        out->level_valid = true;
        out->level = fabsf(out->pitch_deg) <= imu->tilt_threshold_deg &&
                     fabsf(out->roll_deg) <= imu->tilt_threshold_deg;
    }
    return true;
}

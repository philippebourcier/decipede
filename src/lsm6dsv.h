// ST LSM6DSV16X IMU: tilt + vibration, port of lsm6dsv.py.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t addr;
    float tilt_threshold_deg;
    float vibration_threshold_dps;
    uint32_t vibration_window_ms;
} lsm6dsv_t;

typedef struct {
    bool vibrating;
    float rms_max_dps;
    bool level_valid;     // false when vibrating (tilt not measured)
    bool level;
    float pitch_deg;
    float roll_deg;
} imu_check_t;

// Verify WHO_AM_I and configure (XL ±2g @30 Hz, GY ±250 dps @120 Hz).
bool lsm6dsv_init(lsm6dsv_t *imu);
// Vibration RMS over the window, then tilt if calm. Blocks ~1-2 s.
bool lsm6dsv_check(lsm6dsv_t *imu, imu_check_t *out);

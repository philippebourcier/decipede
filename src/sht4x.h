// Sensirion SHT40 temperature / humidity, port of sht4x.py.
#pragma once

#include <stdbool.h>

#define SHT40_ADDR 0x44

// High-precision measurement (~10 ms). Returns false on I2C or CRC error.
bool sht40_read(float *temperature_c, float *humidity_pct);

// Pin map of the RTK base station (see uPyRTKBase README).
#pragma once

// UM980 COM1: control (UART0)
#define UM980_CTRL_UART      uart0
#define UM980_CTRL_TX_PIN    0
#define UM980_CTRL_RX_PIN    1
// UM980 COM2: RTCM data (UART1)
#define UM980_DATA_UART      uart1
#define UM980_DATA_TX_PIN    8
#define UM980_DATA_RX_PIN    9
#define UM980_BAUDRATE       115200

// Shared I2C bus: LSM6DSV16X (0x6B) + SHT40 (0x44)
#define SENSOR_I2C           i2c0
#define SENSOR_I2C_SDA_PIN   12
#define SENSOR_I2C_SCL_PIN   13
#define SENSOR_I2C_HZ        400000

// W5500 on SPI0
#define W5500_SPI            spi0
#define W5500_MISO_PIN       16
#define W5500_CS_PIN         17
#define W5500_SCK_PIN        18
#define W5500_MOSI_PIN       19
#define W5500_RST_PIN        20
#define W5500_INT_PIN        21
#define W5500_SPI_HZ         (33 * 1000 * 1000)

// RGB LEDs, common anode to 3V3 (active low)
#define LED1_R_PIN           28
#define LED1_G_PIN           27
#define LED1_B_PIN           29
#define LED2_R_PIN           25
#define LED2_G_PIN           24
#define LED2_B_PIN           23

// BTN_USER, external 10K pull-down: active high
#define BTN_USER_PIN         26

// UM980 auxiliary signals (wired by the integrator)
#define GNSS_PPS_PIN         2   // RTK_TPS: 1 pulse per second, rising edge = second
#define GNSS_RTK_STAT_PIN    3   // RTK_STAT: high = RTK fixed solution
#define GNSS_RESET_PIN       4   // RTK_RESET: active low, pulled up by the UM980
#define GNSS_EXTINT_PIN      5   // RTK_EXTINT: UM980 EVENT input (unused, high-Z)

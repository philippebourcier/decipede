// WIZnet W5500-EVB-Pico2 as fitted on the RTK base station.
// The pico-sdk has no header for this board; it is a Pico 2 (RP2350A)
// with a W5500 on SPI0 and 16 MiB of QSPI flash (verified by flash dump).

#ifndef _BOARDS_W5500_EVB_PICO2_H
#define _BOARDS_W5500_EVB_PICO2_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define PICO_RP2350A 1

// UART0 drives the UM980 control port, so there is no default UART.
// The console is USB CDC.
#define PICO_DEFAULT_I2C 0
#define PICO_DEFAULT_I2C_SDA_PIN 12
#define PICO_DEFAULT_I2C_SCL_PIN 13

#define PICO_DEFAULT_SPI 0
#define PICO_DEFAULT_SPI_RX_PIN 16
#define PICO_DEFAULT_SPI_CSN_PIN 17
#define PICO_DEFAULT_SPI_SCK_PIN 18
#define PICO_DEFAULT_SPI_TX_PIN 19

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif

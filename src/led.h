// RGB LED driver (PWM, common anode / active low), port of rgb_led_drv.py.
#pragma once

#include <stdint.h>

typedef enum {
    COLOR_OFF,
    COLOR_WHITE,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_MAGENTA,
    COLOR_ORANGE,
    COLOR_PURPLE,
    COLOR_PINK,
    COLOR_COUNT
} color_t;

typedef enum { LED1 = 0, LED2 = 1 } led_id_t;

void led_init(void);
void led_set(led_id_t led, color_t color);
void led_set_rgb(led_id_t led, uint8_t r, uint8_t g, uint8_t b);
void led_both(color_t color);
void led_off(void);
const char *color_name(color_t color);

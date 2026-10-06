#include "led.h"

#include "board.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#define PWM_FREQ_HZ 1000
#define PWM_WRAP    65535

static const uint8_t led_pins[2][3] = {
    {LED1_R_PIN, LED1_G_PIN, LED1_B_PIN},
    {LED2_R_PIN, LED2_G_PIN, LED2_B_PIN},
};

static const struct {
    const char *name;
    uint8_t r, g, b;
} colors[COLOR_COUNT] = {
    [COLOR_OFF]     = {"OFF",       0,   0,   0},
    [COLOR_WHITE]   = {"WHITE",   255, 255, 255},
    [COLOR_RED]     = {"RED",     255,   0,   0},
    [COLOR_GREEN]   = {"GREEN",     0, 255,   0},
    [COLOR_BLUE]    = {"BLUE",      0,   0, 255},
    [COLOR_YELLOW]  = {"YELLOW",  255, 255,   0},
    [COLOR_CYAN]    = {"CYAN",      0, 255, 255},
    [COLOR_MAGENTA] = {"MAGENTA", 255,   0, 255},
    [COLOR_ORANGE]  = {"ORANGE",  255, 128,   0},
    [COLOR_PURPLE]  = {"PURPLE",  128,   0, 255},
    [COLOR_PINK]    = {"PINK",    255,  20, 147},
};

static void write_channel(uint pin, uint8_t value) {
    // Active low: full brightness = 0 duty.
    uint32_t level = (uint32_t)(255 - value) * PWM_WRAP / 255;
    pwm_set_gpio_level(pin, (uint16_t)level);
}

void led_init(void) {
    float div = (float)clock_get_hz(clk_sys) / (PWM_FREQ_HZ * (PWM_WRAP + 1.0f));
    for (int l = 0; l < 2; l++) {
        for (int c = 0; c < 3; c++) {
            uint pin = led_pins[l][c];
            gpio_set_function(pin, GPIO_FUNC_PWM);
            uint slice = pwm_gpio_to_slice_num(pin);
            pwm_config cfg = pwm_get_default_config();
            pwm_config_set_clkdiv(&cfg, div);
            pwm_config_set_wrap(&cfg, PWM_WRAP);
            pwm_init(slice, &cfg, false);
            write_channel(pin, 0);
            pwm_set_enabled(slice, true);
        }
    }
}

void led_set_rgb(led_id_t led, uint8_t r, uint8_t g, uint8_t b) {
    write_channel(led_pins[led][0], r);
    write_channel(led_pins[led][1], g);
    write_channel(led_pins[led][2], b);
}

void led_set(led_id_t led, color_t color) {
    if (color >= COLOR_COUNT) color = COLOR_OFF;
    led_set_rgb(led, colors[color].r, colors[color].g, colors[color].b);
}

void led_both(color_t color) {
    led_set(LED1, color);
    led_set(LED2, color);
}

void led_off(void) {
    led_both(COLOR_OFF);
}

const char *color_name(color_t color) {
    return color < COLOR_COUNT ? colors[color].name : "OFF";
}

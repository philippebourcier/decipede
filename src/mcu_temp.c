#include "mcu_temp.h"

#include <stdbool.h>

#include "hardware/adc.h"

float mcu_temp_read(void) {
    static bool ready;
    if (!ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        ready = true;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += adc_read();
    float v = (float)sum / 16.0f * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;  // RP2350 datasheet, temperature sensor
}

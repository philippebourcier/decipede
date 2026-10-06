// RP2350 internal temperature sensor (ADC), core 0 only.
#pragma once

// Degrees C, averaged over a few conversions. Accuracy is a few degrees: the
// sensor is uncalibrated and the ADC reference is the 3.3 V rail.
float mcu_temp_read(void);

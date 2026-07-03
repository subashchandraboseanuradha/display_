#pragma once
#include <Arduino.h>

// Battery monitor via external voltage divider (100k/100k) from BAT+ to GPIO1 (A0/D0).
// ADC reads half of battery voltage; analogReadMilliVolts() gives calibrated mV.

#define BATTERY_ADC_PIN A0

void battery_init();

// Returns battery voltage in millivolts (full pack voltage, divider-corrected).
uint32_t battery_read_mv();

// Returns estimated charge percent (0-100) for a single-cell Li-ion/LiPo.
int battery_read_percent();

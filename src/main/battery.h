#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "battery_model.h"

/* GPIO10 / ADC1 channel 9, calibrated input voltage x2 for the Cardputer
 * V1.1 voltage divider. No charger/current-detect hardware exposed to MCU.
 */
esp_err_t battery_init(void);
/* Release ADC hardware while the SAR-based RNG entropy source is active. */
esp_err_t battery_suspend(void);
bool battery_tick(uint32_t now_ms);
battery_status_t battery_get_status(void);

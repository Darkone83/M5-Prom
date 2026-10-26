#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Cardputer V1.1 exposes ONLY Li-ion voltage on GPIO10, not charge current
 * or a wired charger-state pin. These trends describe VOLTAGE, not confirmed
 * charger activity. Percentage is a nominal single-cell 1S Li-ion estimate.
 */
typedef enum {
    BATTERY_TREND_UNKNOWN = 0,
    BATTERY_TREND_RISING,
    BATTERY_TREND_FALLING,
    BATTERY_TREND_STABLE
} battery_trend_t;

typedef struct {
    bool valid;
    int millivolts;
    int percentage;
    battery_trend_t trend;
} battery_status_t;

typedef struct {
    int ring[5];
    unsigned count;
    unsigned index;
    int filtered_mv;
    int trend_baseline_mv;
    uint32_t trend_baseline_ms;
    bool trend_initialized;
    battery_status_t status;
} battery_model_t;

void battery_model_init(battery_model_t *model);
/* Returns true if percentage, trend, or voltage rounded to 10mV changed. */
bool battery_model_sample(battery_model_t *model, int voltage_mv, uint32_t time_ms);
int battery_estimate_percent(int voltage_mv);

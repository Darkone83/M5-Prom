#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"
/* ESP-IDF's pdMS_TO_TICKS() truncates: at 100 Hz a 1 ms delay becomes 0.
 * Always block at least one kernel tick when a positive wait is requested. */
static inline TickType_t m5_remote_wait_ticks(uint32_t milliseconds)
{
    TickType_t ticks=pdMS_TO_TICKS(milliseconds);
    return ticks ? ticks : 1;
}

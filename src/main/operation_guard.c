#include "operation_guard.h"
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_gate;

bool operation_guard_init(void)
{
    /* Called once by app_main before the HTTP server is started. */
    if (s_gate) return true;
    s_gate = xSemaphoreCreateMutex();
    return s_gate != NULL;
}

bool operation_guard_try_acquire(void)
{
    return s_gate && xSemaphoreTake(s_gate, 0) == pdTRUE;
}

void operation_guard_release(void)
{
    if (s_gate) (void)xSemaphoreGive(s_gate);
}

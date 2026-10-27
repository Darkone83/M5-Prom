#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Vendor-specific USB-OTG transport: never invokes the UART/Serial-JTAG API. */
esp_err_t remote_native_init(void);
bool remote_native_connected(void);
int remote_native_read(uint8_t *dst, size_t capacity);
/* USB callbacks wake the remote task; no zero-tick high-priority polling. */
void remote_native_set_task(TaskHandle_t task);
void remote_native_wake(void);
void remote_native_wait(uint32_t timeout_ms);
uint32_t remote_native_sent_bytes(void);
bool remote_native_write_packet(const uint8_t *src, size_t size, uint32_t timeout_ms);
/* Frame transport: enqueue a batch of complete v1 packets atomically, with
 * enough FIFO headroom left for control/keyboard acknowledgements. Never waits.
 * TinyUSB flushes this entire byte stream as one submission. */
bool remote_native_write_screen_batch(const uint8_t *src, size_t size);

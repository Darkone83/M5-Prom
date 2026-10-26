#pragma once
#include <stdbool.h>
#include <stdint.h>

// Minimal persistent UI configuration; independent of Wi-Fi credentials.
void settings_init(void);
uint8_t settings_brightness(void);
void settings_set_brightness(uint8_t percent);
uint16_t settings_dim_seconds(void);
void settings_set_dim_seconds(uint16_t seconds);
bool settings_web_enabled(void);
void settings_set_web_enabled(bool enabled);

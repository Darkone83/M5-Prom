#pragma once
#include <stdint.h>
#include "esp_err.h"

// Cardputer V1.1 240x135 palette: near-black surfaces with violet + teal accents.
#define UI_BG          0x0C0E18u
#define UI_HEADER      0x111321u
#define UI_PANEL       0x191B2Du
#define UI_FOCUS       0x302641u
#define UI_LINE        0x303047u
#define UI_ACCENT      0xB381FFu
#define UI_ACCENT_DIM  0x6D498Fu
#define UI_CYAN        0x65D8E5u
#define UI_WHITE       0xF5F0FBu
#define UI_MUTED       0xAAA7BCu
#define UI_GOOD        0x66E1B6u
#define UI_BAD         0xFF718Cu
#define UI_WARN        0xFFC278u

esp_err_t display_init(void);
// PWM backlight (GPIO38 also supplies RGB LED power). Range 5..100 percent.
esp_err_t display_set_brightness(uint8_t percent);
uint8_t display_get_brightness(void);
void display_clear(uint32_t rgb);
void display_rect(int x, int y, int w, int h, uint32_t rgb);
void display_frame(int x, int y, int w, int h, uint32_t rgb);
void display_text(int x, int y, const char *text, uint32_t rgb, int scale);
/* Copies the flash-resident M5 Prom RGB565 splash into the existing framebuffer. */
void display_show_splash(void);
void display_present(void);

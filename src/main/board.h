#pragma once

// This map is for the M5Stack Cardputer V1.1 (K132-V11 / Stamp-S3A), NOT Cardputer ADV.
#define CP_LCD_MOSI 35
#define CP_LCD_SCLK 36
#define CP_LCD_CS   37
#define CP_LCD_DC   34
#define CP_LCD_RST  33
#define CP_LCD_BL   38

#define CP_SD_MOSI  14
#define CP_SD_MISO  39
#define CP_SD_SCLK  40
#define CP_SD_CS    12

/* Cardputer V1.1 NS4168 I2S speaker (GPIO43 shares mic clock). */
#define CP_SPK_BCLK 41
#define CP_SPK_DATA 42
#define CP_SPK_LRCLK 43

#define CP_BAT_ADC_GPIO 10  // ADC1 CH9; 2:1 divider from battery.

#define CP_GROVE_SCL 1
#define CP_GROVE_SDA 2

#define CP_LCD_WIDTH  240
#define CP_LCD_HEIGHT 135

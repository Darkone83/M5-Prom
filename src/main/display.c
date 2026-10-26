#include "display.h"
#include "splash_asset.h"
#include "board.h"
#include <string.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t panel;
static bool s_pwm_ready;
static uint8_t s_brightness=100;
static SemaphoreHandle_t s_tx_done;
static uint16_t fb[CP_LCD_WIDTH * CP_LCD_HEIGHT];
#define STRIP_ROWS 10
static uint16_t *s_transfer;

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *evt, void *ctx)
{
    (void)io;
    (void)evt;
    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &higher);
    return higher == pdTRUE;
}

static uint16_t wire565(uint32_t rgb)
{
    const uint16_t r = ((rgb >> 16) & 0xffu) >> 3;
    const uint16_t g = ((rgb >> 8) & 0xffu) >> 2;
    const uint16_t b = (rgb & 0xffu) >> 3;
    const uint16_t pix = (r << 11) | (g << 5) | b;
    // Swap bytes for ST7789, which expects RGB565 in MSB-first order.
    return (uint16_t)((pix << 8) | (pix >> 8));
}

esp_err_t display_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = CP_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CP_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CP_LCD_WIDTH * STRIP_ROWS * 2 + 8,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    s_tx_done = xSemaphoreCreateBinary();
    s_transfer = heap_caps_malloc(CP_LCD_WIDTH * STRIP_ROWS * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_tx_done || !s_transfer) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CP_LCD_DC,
        .cs_gpio_num = CP_LCD_CS,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_color_done,
        .user_ctx = s_tx_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) return err;
    const esp_lcd_panel_dev_config_t lcd_cfg = {
        .reset_gpio_num = CP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &lcd_cfg, &panel);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 40, 52));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_reset_pin((gpio_num_t)CP_LCD_BL);
    gpio_set_direction((gpio_num_t)CP_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CP_LCD_BL, 1);
    const ledc_timer_config_t timer = {
        .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_10_BIT,
        .timer_num=LEDC_TIMER_0, .freq_hz=4000, .clk_cfg=LEDC_AUTO_CLK
    };
    const ledc_channel_config_t channel = {
        .gpio_num=CP_LCD_BL, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0, .intr_type=LEDC_INTR_DISABLE,
        .timer_sel=LEDC_TIMER_0, .duty=1023, .hpoint=0
    };
    if(ledc_timer_config(&timer)==ESP_OK && ledc_channel_config(&channel)==ESP_OK)
        s_pwm_ready=true;
    else ESP_LOGW(TAG,"PWM backlight unavailable; staying at full brightness");
    display_clear(UI_BG);
    display_present();
    ESP_LOGI(TAG, "LCD initialized, 240x135 landscape (V1.1)");
    return ESP_OK;
}

esp_err_t display_set_brightness(uint8_t percent)
{
    if(percent<5)percent=5;
    if(percent>100)percent=100;
    if(!s_pwm_ready)return ESP_ERR_INVALID_STATE;
    const uint32_t duty=(1023u*percent+50u)/100u;
    esp_err_t e=ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,duty);
    if(e==ESP_OK)e=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    if(e==ESP_OK)s_brightness=percent;
    return e;
}
uint8_t display_get_brightness(void){return s_brightness;}

void display_show_splash(void)
{
    /* The asset is already converted to the ST7789 wire byte order. */
    _Static_assert(sizeof(fb) == sizeof(m5prom_splash_pixels), "splash mismatch");
    memcpy(fb, m5prom_splash_pixels, sizeof(fb));
    display_present();
}

void display_clear(uint32_t rgb)
{
    uint16_t color = wire565(rgb);
    for (int i = 0; i < CP_LCD_WIDTH * CP_LCD_HEIGHT; ++i) fb[i] = color;
}

void display_rect(int x, int y, int w, int h, uint32_t rgb)
{
    int endx = x + w, endy = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (endx > CP_LCD_WIDTH) endx = CP_LCD_WIDTH;
    if (endy > CP_LCD_HEIGHT) endy = CP_LCD_HEIGHT;
    if (endx <= x || endy <= y) return;
    uint16_t color = wire565(rgb);
    for (int row = y; row < endy; ++row) {
        for (int col = x; col < endx; ++col) fb[row * CP_LCD_WIDTH + col] = color;
    }
}

// Simple 1px perimeter; clipped by display_rect for safe edge draws.
void display_frame(int x, int y, int w, int h, uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    display_rect(x, y, w, 1, rgb);
    if (h > 1) display_rect(x, y+h-1, w, 1, rgb);
    if (h > 2) {
        display_rect(x, y+1, 1, h-2, rgb);
        if (w > 1) display_rect(x+w-1, y+1, 1, h-2, rgb);
    }
}

// Minimal locally defined 5x7 uppercase font. Each byte is one column; LSB is top pixel.
// Compact pixel glyphs are intentional: 6px per character at native resolution.
typedef struct { char c; uint8_t b[5]; } glyph_t;
static const glyph_t glyphs[] = {
    {'A',{0x7e,0x11,0x11,0x11,0x7e}}, {'B',{0x7f,0x49,0x49,0x49,0x36}},
    {'C',{0x3e,0x41,0x41,0x41,0x22}}, {'D',{0x7f,0x41,0x41,0x22,0x1c}},
    {'E',{0x7f,0x49,0x49,0x49,0x41}}, {'F',{0x7f,0x09,0x09,0x09,0x01}},
    {'G',{0x3e,0x41,0x49,0x49,0x7a}}, {'H',{0x7f,0x08,0x08,0x08,0x7f}},
    {'I',{0x41,0x41,0x7f,0x41,0x41}}, {'J',{0x20,0x40,0x41,0x3f,0x01}},
    {'K',{0x7f,0x08,0x14,0x22,0x41}}, {'L',{0x7f,0x40,0x40,0x40,0x40}},
    {'M',{0x7f,0x02,0x0c,0x02,0x7f}}, {'N',{0x7f,0x04,0x08,0x10,0x7f}},
    {'O',{0x3e,0x41,0x41,0x41,0x3e}}, {'P',{0x7f,0x09,0x09,0x09,0x06}},
    {'Q',{0x3e,0x41,0x51,0x21,0x5e}}, {'R',{0x7f,0x09,0x19,0x29,0x46}},
    {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7f,0x01,0x01}},
    {'U',{0x3f,0x40,0x40,0x40,0x3f}}, {'V',{0x1f,0x20,0x40,0x20,0x1f}},
    {'W',{0x7f,0x20,0x18,0x20,0x7f}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
    {'Y',{0x03,0x04,0x78,0x04,0x03}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
    {'0',{0x3e,0x51,0x49,0x45,0x3e}}, {'1',{0x00,0x42,0x7f,0x40,0x00}},
    {'2',{0x62,0x51,0x49,0x49,0x46}}, {'3',{0x22,0x49,0x49,0x49,0x36}},
    {'4',{0x18,0x14,0x12,0x7f,0x10}}, {'5',{0x2f,0x49,0x49,0x49,0x31}},
    {'6',{0x3e,0x49,0x49,0x49,0x32}}, {'7',{0x01,0x71,0x09,0x05,0x03}},
    {'8',{0x36,0x49,0x49,0x49,0x36}}, {'9',{0x26,0x49,0x49,0x49,0x3e}},
    {'.',{0x00,0x60,0x60,0x00,0x00}}, {':',{0x00,0x36,0x36,0x00,0x00}},
    {'-',{0x08,0x08,0x08,0x08,0x08}}, {'_',{0x40,0x40,0x40,0x40,0x40}},
    {'/',{0x60,0x10,0x08,0x04,0x03}}, {'+', {0x08,0x08,0x3e,0x08,0x08}},
    {'=',{0x14,0x14,0x14,0x14,0x14}}, {'?',{0x02,0x01,0x51,0x09,0x06}},
    {'!',{0x00,0x00,0x5f,0x00,0x00}}, {' ',{0,0,0,0,0}},
    {'(',{0x00,0x1c,0x22,0x41,0x00}}, {')',{0x00,0x41,0x22,0x1c,0x00}},
    {'>',{0x41,0x22,0x14,0x08,0x00}}, {'<',{0x00,0x08,0x14,0x22,0x41}},
    {'#',{0x14,0x7f,0x14,0x7f,0x14}}, {'|',{0x00,0x00,0x7f,0x00,0x00}},
    {'*',{0x14,0x08,0x3e,0x08,0x14}},
    {'%',{0x63,0x13,0x08,0x64,0x63}},
    {'`',{0x00,0x01,0x02,0x00,0x00}},
    {',',{0x00,0x40,0x20,0x00,0x00}},
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i)
        if (glyphs[i].c == c) return glyphs[i].b;
    return glyphs[sizeof(glyphs)/sizeof(glyphs[0])-1].b; // unknown chars shown as '*'
}

void display_text(int x, int y, const char *text, uint32_t rgb, int scale)
{
    if (!text || scale < 1 || scale > 3) return;
    for (const char *p = text; *p; ++p) {
        const uint8_t *g = glyph(*p);
        for (int col=0; col<5; ++col) {
            for (int row=0; row<7; ++row) {
                if (g[col] & (1u << row))
                    display_rect(x + col*scale, y + row*scale, scale, scale, rgb);
            }
        }
        x += 6*scale;
        if (x >= CP_LCD_WIDTH) return;
    }
}

void display_present(void)
{
    if (!panel || !s_transfer) return;
    for (int y=0; y<CP_LCD_HEIGHT; y+=STRIP_ROWS) {
        int nrows = (CP_LCD_HEIGHT - y < STRIP_ROWS) ? CP_LCD_HEIGHT - y : STRIP_ROWS;
        memcpy(s_transfer, &fb[y * CP_LCD_WIDTH], nrows * CP_LCD_WIDTH * sizeof(uint16_t));
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, CP_LCD_WIDTH, y+nrows, s_transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Display transfer failed: %s", esp_err_to_name(err));
            break;
        }
        if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Display DMA timed out");
            break;
        }
    }
}

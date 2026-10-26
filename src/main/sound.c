/*
 * Cardputer V1.1 (not ADV) event chimes via NS4168 I2S speaker.
 * BCLK GPIO41, SDATA GPIO42, LRCLK GPIO43. GPIO43 is shared with the mic.
 * No WAV assets, PSRAM, legacy I2S driver, or timing delay in UI task.
 */
#include "sound.h"
#include "sound_wave.h"
#include "board.h"
#include <stdint.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cardputer_sound";
static i2s_chan_handle_t s_tx = NULL;
static QueueHandle_t s_events = NULL;
static volatile bool s_enabled = true;
static bool s_ready = false;
static bool s_nvs_ready = false;

/* A one-item mailbox means a burst of UI results cannot stack up beeps. */
typedef enum { CHIME_OK = 1, CHIME_FAIL = 2 } chime_t;

static void load_preference(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s); sound setting will be temporary", esp_err_to_name(err));
        return; /* Do not erase NVS in order to recover one optional setting. */
    }
    s_nvs_ready = true;
    nvs_handle_t handle;
    if (nvs_open("ui_audio", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t enabled = 1;
        if (nvs_get_u8(handle, "enabled", &enabled) == ESP_OK)
            s_enabled = (enabled != 0);
        nvs_close(handle);
    }
}

static bool send_frames(const int16_t *samples, size_t frames)
{
    size_t written = 0;
    const size_t bytes = frames * 2u * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(s_tx, samples, bytes, &written, 200);
    if (err != ESP_OK || written != bytes) {
        ESP_LOGW(TAG, "I2S send failed: %s, %u/%u bytes", esp_err_to_name(err),
                 (unsigned)written, (unsigned)bytes);
        return false;
    }
    return true;
}

static bool play_silence(uint16_t duration_ms)
{
    const int16_t zeroes[128u * 2u] = {0};
    size_t remaining = ((size_t)SOUND_SAMPLE_RATE * duration_ms) / 1000u;
    while (remaining) {
        size_t frames = remaining < 128u ? remaining : 128u;
        if (!send_frames(zeroes, frames)) return false;
        remaining -= frames;
    }
    return true;
}

static bool play_note(uint16_t hz, uint16_t duration_ms)
{
    int16_t pcm[128u * 2u];
    sound_wave_t wave;
    sound_wave_begin(&wave, hz, duration_ms);
    while (wave.produced < wave.total && s_enabled) {
        size_t frames = sound_wave_render(&wave, pcm, 128u);
        if (frames == 0 || !send_frames(pcm, frames)) return false;
    }
    return s_enabled;
}

static void sound_task(void *arg)
{
    (void)arg;
    chime_t chime;
    for (;;) {
        if (xQueueReceive(s_events, &chime, portMAX_DELAY) != pdTRUE) continue;
        if (!s_enabled) continue;
        esp_err_t err = i2s_channel_enable(s_tx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S start failed: %s", esp_err_to_name(err));
            continue;
        }
        /* Drive silence before first note; no I2S clocks are needed when idle. */
        bool ok = play_silence(12);
        if (ok && s_enabled) {
            if (chime == CHIME_OK)
                ok = play_note(880, 60) && play_silence(18) && play_note(1320, 88);
            else
                ok = play_note(660, 100) && play_silence(28) && play_note(440, 150);
        }
        /* Finish with silence and let DMA drain before stopping the clocks. */
        (void)ok;
        (void)play_silence(32);
        vTaskDelay(pdMS_TO_TICKS(24));
        (void)i2s_channel_disable(s_tx);
    }
}

bool sound_init(void)
{
    load_preference();

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 128;
    esp_err_t err = i2s_new_channel(&channel, &s_tx, NULL);
    if (err != ESP_OK) goto failed;

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOUND_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CP_SPK_BCLK,
            .ws = CP_SPK_LRCLK,
            .dout = CP_SPK_DATA,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err != ESP_OK) goto failed;
    /* Leave channel in READY state; the audio task enables it per chime. */
    s_events = xQueueCreate(1, sizeof(chime_t));
    if (!s_events) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    if (xTaskCreate(sound_task, "ui_chime", 3072, NULL, 3, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto failed_queue;
    }
    s_ready = true;
    ESP_LOGI(TAG, "NS4168 event chimes %s", s_enabled ? "enabled" : "muted");
    return true;

failed_queue:
    vQueueDelete(s_events);
    s_events = NULL;
failed:
    ESP_LOGW(TAG, "Speaker init failed (%s); continuing without sound", esp_err_to_name(err));
    if (s_tx) (void)i2s_del_channel(s_tx);
    s_tx = NULL;
    return false;
}

bool sound_available(void) { return s_ready; }
bool sound_enabled(void) { return s_enabled && s_ready; }

void sound_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!s_nvs_ready) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("ui_audio", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "enabled", enabled ? 1u : 0u);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Unable to save mute preference: %s", esp_err_to_name(err));
}

void sound_notify(bool success)
{
    if (!sound_enabled() || !s_events) return;
    const chime_t chime = success ? CHIME_OK : CHIME_FAIL;
    (void)xQueueOverwrite(s_events, &chime);
}

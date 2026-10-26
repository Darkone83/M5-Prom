#pragma once

/* Pure C PCM tone generator. No ESP-IDF dependency and no heap allocations. */
#include <stddef.h>
#include <stdint.h>

#define SOUND_SAMPLE_RATE 16000u

typedef struct {
    uint32_t phase;
    uint32_t produced;
    uint32_t total;
    uint16_t frequency;
} sound_wave_t;

void sound_wave_begin(sound_wave_t *wave, uint16_t frequency_hz, uint16_t duration_ms);
size_t sound_wave_render(sound_wave_t *wave, int16_t *stereo, size_t frame_capacity);

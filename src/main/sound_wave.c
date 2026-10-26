/* Quiet triangle-wave chimes, with a short attack/release to avoid clicks. */
#include "sound_wave.h"

void sound_wave_begin(sound_wave_t *wave, uint16_t frequency_hz, uint16_t duration_ms)
{
    if (!wave) return;
    wave->phase = 0;
    wave->produced = 0;
    wave->frequency = frequency_hz;
    wave->total = (SOUND_SAMPLE_RATE * (uint32_t)duration_ms) / 1000u;
}

size_t sound_wave_render(sound_wave_t *wave, int16_t *stereo, size_t frame_capacity)
{
    if (!wave || !stereo || !frame_capacity || !wave->frequency) return 0;
    const uint32_t remaining = wave->total > wave->produced ? wave->total - wave->produced : 0u;
    if (frame_capacity > remaining) frame_capacity = remaining;

    /* Moderate ~15% full-scale peak; envelope prevents harsh clicks. */
    const int32_t amplitude = 5000;
    const uint32_t fade_frames = SOUND_SAMPLE_RATE / 125u; /* 8 milliseconds */
    for (size_t i = 0; i < frame_capacity; ++i) {
        uint32_t phase = wave->phase + wave->frequency;
        if (phase >= SOUND_SAMPLE_RATE) phase %= SOUND_SAMPLE_RATE;
        wave->phase = phase;

        /* Triangle wave, -1 to +1, entirely integer arithmetic. */
        int32_t triangle = (phase < SOUND_SAMPLE_RATE / 2u)
            ? (int32_t)(phase * 4u) - (int32_t)SOUND_SAMPLE_RATE
            : (int32_t)(3u * SOUND_SAMPLE_RATE) - (int32_t)(phase * 4u);
        int32_t pcm = (triangle * amplitude) / (int32_t)SOUND_SAMPLE_RATE;

        /* Envelope with no jumps at either end, even for short notes. */
        uint32_t index = wave->produced;
        uint32_t left = wave->total - 1u - index;
        uint32_t env = index < left ? index : left;
        if (env > fade_frames) env = fade_frames;
        pcm = (pcm * (int32_t)env) / (int32_t)fade_frames;

        /* Send to both slots: NS4168 is mono with hardware channel selection. */
        stereo[i * 2u] = (int16_t)pcm;
        stereo[i * 2u + 1u] = (int16_t)pcm;
        wave->produced++;
    }
    return frame_capacity;
}

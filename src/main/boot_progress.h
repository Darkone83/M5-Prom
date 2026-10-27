#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Real completed initialization milestones, not elapsed-time estimates.
 * Progress is monotonic and never reaches 100% before boot finishes.
 * Keep this header independent of ESP-IDF so the milestones are host-testable.
 */
typedef enum {
    M5_BOOT_START = 0,
    M5_BOOT_SETTINGS,
    M5_BOOT_KEYBOARD,
    M5_BOOT_SOUND,
    M5_BOOT_STORAGE,
    M5_BOOT_EEPROM,
    M5_BOOT_WIFI,
    M5_BOOT_BATTERY,
    M5_BOOT_USB,
    M5_BOOT_READY,
    M5_BOOT_STAGE_COUNT
} m5_boot_stage_t;

static inline uint8_t m5_boot_percent(m5_boot_stage_t stage)
{
    static const uint8_t completed[M5_BOOT_STAGE_COUNT] = {
        0, 12, 21, 29, 47, 65, 81, 90, 97, 100
    };
    return (unsigned)stage < M5_BOOT_STAGE_COUNT ? completed[stage] : 0;
}

static inline const char *m5_boot_text(m5_boot_stage_t stage, bool ok)
{
    switch (stage) {
        case M5_BOOT_START: return "INITIALIZING";
        case M5_BOOT_SETTINGS: return "SETTINGS LOADED";
        case M5_BOOT_KEYBOARD: return "KEYBOARD READY";
        case M5_BOOT_SOUND: return ok ? "AUDIO READY" : "AUDIO SKIPPED";
        case M5_BOOT_STORAGE: return ok ? "SD CARD READY" : "SD NOT AVAILABLE";
        case M5_BOOT_EEPROM: return ok ? "EEPROM READY" : "EEPROM NOT FOUND";
        case M5_BOOT_WIFI: return ok ? "WI-FI READY" : "WI-FI OFFLINE";
        case M5_BOOT_BATTERY: return ok ? "BATTERY READY" : "BATTERY UNAVAILABLE";
        case M5_BOOT_USB: return ok ? "USB REMOTE READY" : "USB REMOTE OFFLINE";
        case M5_BOOT_READY: return "READY";
        default: return "INITIALIZING";
    }
}

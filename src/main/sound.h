#pragma once

#include <stdbool.h>

/* Nonfatal initialization: UI and EEPROM tools continue if audio is absent. */
bool sound_init(void);
bool sound_available(void);
bool sound_enabled(void);
void sound_set_enabled(bool enabled);   /* persists to NVS if available */
void sound_notify(bool success);       /* nonblocking success/failure cue */

#pragma once
#include <stdint.h>

typedef enum {
    CP_KEY_NONE=0, CP_KEY_CHAR, CP_KEY_UP, CP_KEY_DOWN,
    CP_KEY_LEFT, CP_KEY_RIGHT, CP_KEY_ENTER, CP_KEY_BACKSPACE, CP_KEY_CANCEL
} cp_key_type_t;
typedef struct {cp_key_type_t type;char character;} cp_key_event_t;

void keyboard_init(void);
// Edge-triggered, debounced events. Non-modifier chars use physical V1.1 layout.
// Fn+;/. , / are arrows; Fn+` is cancel; no key auto-repeat in v0.2.
cp_key_event_t keyboard_poll(void);

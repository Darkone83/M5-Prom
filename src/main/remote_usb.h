#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "keyboard.h"

/* Nonfatal: if USB transport is unavailable the Cardputer remains standalone. */
esp_err_t remote_usb_init(void);
bool remote_usb_active(void);
/* Called from the normal UI task. Copies framebuffer; no LCD or USB operations. */
void remote_usb_publish(const uint16_t *fb);
/* Returns CP_KEY_NONE if no input. PC events use the real handle_key() path. */
cp_key_event_t remote_usb_poll_key(void);
/* Pass 1 diagnostics: optional key sequence, used only when host opts in. */
cp_key_event_t remote_usb_poll_key_with_sequence(uint16_t *sequence);
/* Called from main UI after handle_key() and display_present() have returned. */
void remote_usb_key_rendered(uint16_t sequence);

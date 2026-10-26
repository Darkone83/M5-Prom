#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DONOR_RANDOM_SIZE 54

/* Retail EEPROM security format: 1 = 1.0, 2 = 1.1-1.4, 3 = 1.6/1.6b.
   0 is Debug/Chihiro, intentionally unavailable for retail donor creation.
   The caller MUST supply freshly generated random bytes for a real donor. */
const char *donor_revision_label(int version);
bool donor_generate(uint8_t output[256], int version,
                    const uint8_t random_bytes[DONOR_RANDOM_SIZE]);

/* Make a separate raw offline copy of an existing EEPROM image. Preserves
 * HDD key, online key, MAC, serial, original encryption/IV and other fields.
 * Deliberately does not 'repair' or write it; validation is performed later.
 */
bool donor_copy_image(uint8_t output[256],const uint8_t source[256]);

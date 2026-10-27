#pragma once
#include <stdint.h>
#include <stddef.h>

// Port of Darkone83/XbDiag's pure EEPROM security-section routines.
// Data is a RAW encrypted EEPROM image unless explicitly documented.
// Caller supplies all 256 bytes; no kernel APIs, I2C or persistence involved.
// Returns security-IV index 0-3 on successful HMAC verification, -1 otherwise.
int image_crypto_decrypt(uint8_t buffer[256]);
// ENCRYPTS decrypted security section in-place. Use only with a verified index.
void image_crypto_encrypt(uint8_t buffer[256], int version);

/* Standard HMAC-SHA1 (not Xbox's version-specific EEPROM HMAC).
 * Two message pieces are hashed consecutively: ATA model then ATA serial.
 * No dynamic allocation or dependency on Xbox kernel libraries.
 */
void image_crypto_hmac_sha1_two(const uint8_t *key, size_t key_len,
                                const uint8_t *part1, size_t len1,
                                const uint8_t *part2, size_t len2,
                                uint8_t digest[20]);

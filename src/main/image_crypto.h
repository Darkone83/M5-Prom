#pragma once
#include <stdint.h>

// Port of Darkone83/XbDiag's pure EEPROM security-section routines.
// Data is a RAW encrypted EEPROM image unless explicitly documented.
// Caller supplies all 256 bytes; no kernel APIs, I2C or persistence involved.
// Returns security-IV index 0-3 on successful HMAC verification, -1 otherwise.
int image_crypto_decrypt(uint8_t buffer[256]);
// ENCRYPTS decrypted security section in-place. Use only with a verified index.
void image_crypto_encrypt(uint8_t buffer[256], int version);

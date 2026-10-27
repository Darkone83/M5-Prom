#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* These routines operate on a RAW, encrypted 256-byte Xbox EEPROM image.
 * A failed security HMAC prevents displaying or exporting an untrusted key.
 * No Xbox / ATA device I/O is performed by this module.
 */
#define HDD_RECOVERY_KEY_HEX_SIZE 33
#define HDD_RECOVERY_REPORT_MAX 1024
#define HDD_DRIVE_MODEL_MAX 40
#define HDD_DRIVE_SERIAL_MAX 20
#define HDD_ATA_PASSWORD_BYTES 32
#define HDD_ATA_PASSWORD_HEX_SIZE 65

bool hdd_recovery_key_hex(const uint8_t raw[256], char hex[HDD_RECOVERY_KEY_HEX_SIZE]);

/* Build a human-readable, plaintext report. It contains the HDD KEY, not
 * the ATA user password (which requires the physical drive model/serial).
 */
bool hdd_recovery_report(const uint8_t raw[256], bool from_chip, bool is_donor,
                         char *out, size_t out_cap);

/* ATA IDENTIFY's 40-byte model and 20-byte serial are word-swapped by the
 * ATA reader, then only trailing ASCII 0x20 space padding is removed.
 * Strings supplied here must be the exact resulting ASCII characters.
 * Internal/leading spaces and case must be preserved. No HDD I/O.
 */
bool hdd_recovery_ata_fields_valid(const char *model,const char *drive_serial);
bool hdd_recovery_ata_password_hex(const uint8_t raw[256],
                                    const char *model,const char *drive_serial,
                                    char hex[HDD_ATA_PASSWORD_HEX_SIZE]);
/* NULL/empty model + serial keeps the previous HDD-key-only report format;
 * one missing field is rejected instead of exporting a misleading password.
 */
bool hdd_recovery_report_for_drive(const uint8_t raw[256],bool from_chip,bool is_donor,
                                   const char *model,const char *drive_serial,
                                   char *out,size_t out_cap);

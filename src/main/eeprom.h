#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define XBOX_EEPROM_I2C_ADDRESS 0x54
#define XBOX_EEPROM_SIZE 256

/* Events correspond to completed I2C transactions / acknowledged byte writes,
 * never simulated timer progress. The second full read must match before a
 * verified-read event can be emitted. For WRITE, 100% means full readback.
 */
typedef enum {
    EEPROM_PROGRESS_PROBE=0,
    EEPROM_PROGRESS_READ_FIRST,
    EEPROM_PROGRESS_READ_SECOND,
    EEPROM_PROGRESS_READ_MATCH,
    EEPROM_PROGRESS_PROGRAM,
    EEPROM_PROGRESS_VERIFY_FIRST,
    EEPROM_PROGRESS_VERIFY_SECOND,
    EEPROM_PROGRESS_VERIFIED
} eeprom_progress_phase_t;

typedef void (*eeprom_progress_cb_t)(void *user, eeprom_progress_phase_t phase,
                                     unsigned completed, unsigned total);

typedef struct {
    uint16_t different;      // Bytes requiring a write.
    uint16_t acknowledged;   // Writes with a successful post-write ACK.
    bool verified;           // Final two reads matched requested image.
} eeprom_write_status_t;

esp_err_t eeprom_init(void);
esp_err_t eeprom_probe(void);
// Two matching full 24C02 reads. No write-data command.
// Progress callback executes synchronously from the calling UI task.
esp_err_t eeprom_read_verified(uint8_t out[XBOX_EEPROM_SIZE]);
esp_err_t eeprom_read_verified_progress(uint8_t out[XBOX_EEPROM_SIZE],
                                        eeprom_progress_cb_t cb,void *user);
// Re-probes and compares against a freshly saved expected-before image before
// writing. Uses individual byte writes + ACK polling (no page-wrap assumptions).
// On any failure, chip may be PARTIALLY UPDATED; caller must report this.
esp_err_t eeprom_write_verified(const uint8_t desired[XBOX_EEPROM_SIZE],
                                const uint8_t expected_before[XBOX_EEPROM_SIZE],
                                eeprom_write_status_t *status);
esp_err_t eeprom_write_verified_progress(const uint8_t desired[XBOX_EEPROM_SIZE],
                                         const uint8_t expected_before[XBOX_EEPROM_SIZE],
                                         eeprom_write_status_t *status,
                                         eeprom_progress_cb_t cb, void *user);
uint32_t eeprom_crc32(const uint8_t data[XBOX_EEPROM_SIZE]);

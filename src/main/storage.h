#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// SD FAT root: /EEPROMs/<12-digit SN>/ORIGINAL_0001.BIN
// Files are all raw encrypted 256-byte Xbox EEPROM images.
#define STORAGE_MAX_ENTRIES 40
#define STORAGE_NAME_SIZE 32

esp_err_t storage_init(void);
// Real unmount + mount for insertion/replacement, only when Web Files is off.
esp_err_t storage_remount(void);
// Short, human-readable diagnostics for the most recent SD operation.
const char *storage_last_error(void);
const char *storage_last_hint(void);
bool storage_ready(void);
bool storage_mounted(void);
// Last filesystem stage and POSIX errno, for on-device diagnostics.
const char *storage_last_stage(void);
int storage_last_errno(void);
// Non-destructive temporary file write / sync / readback / delete check.
// Existing files and folders are never overwritten, deleted or formatted.
esp_err_t storage_self_test(void);
// Creates /EEPROMs/SN, or /EEPROMs/UNKNOWN_<CRC32> for invalid serial numbers.
esp_err_t storage_folder_for_raw(const uint8_t raw[256],char *folder,size_t len);
// Creates a new file exclusively (never overwrite), reopens and byte-verifies it.
esp_err_t storage_save_version(const char *folder,const char *tag,const uint8_t raw[256],char *name,size_t name_len);
// Names are relative to /EEPROMs, never absolute user-supplied paths.
esp_err_t storage_load_version(const char *folder,const char *name,uint8_t raw[256]);
size_t storage_scan_folders(char names[][STORAGE_NAME_SIZE],size_t maximum);
size_t storage_scan_files(const char *folder,char names[][STORAGE_NAME_SIZE],size_t maximum);

/* Verified, exclusive plain-text export to /EEPROMs/<serial>/HDDKEY_####.TXT.
 * No raw-sector access, overwrites, or writes to the Xbox EEPROM.
 * The caller holds operation_guard and supplies an authenticated report.
 */
esp_err_t storage_save_hdd_report(const char *folder, const char *report, size_t report_len,
                                  char *name, size_t name_len);

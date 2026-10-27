#pragma once
/* Mounted FAT32 VFS operations. Paths are relative to /sdcard.
 * The HANDHELD file manager enforces /EEPROMs as its fixed browsing root;
 * other firmware consumers may use these VFS primitives independently.
 * Never formats, unmounts, remounts, or touches raw SD sectors.
 * Caller must hold operation_guard for every filesystem operation.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "storage.h"

#define SD_BROWSER_NAME_MAX 256
#define SD_BROWSER_DIR_MAX 480
#define SD_BROWSER_PAGE_SIZE 12

typedef struct {
    char name[SD_BROWSER_NAME_MAX];
    bool directory;
    uint64_t size;
} sd_browser_entry_t;

const char *sd_browser_error(void);
bool sd_browser_name_valid(const char *name);
/* Enumerate any normal FAT32 files/folders. Page order is FAT directory order. */
esp_err_t sd_browser_list(const char *dir, size_t offset,
                          sd_browser_entry_t *page, size_t capacity,
                          size_t *page_count, size_t *total);
esp_err_t sd_browser_mkdir(const char *dir, const char *name);
esp_err_t sd_browser_rename(const char *dir, const char *old_name, const char *new_name);
/* Copies normal files or directory trees; auto-generates a unique destination name.
 * No source modifications, no destination overwrite, and verified file readback.
 */
esp_err_t sd_browser_copy(const char *source_dir, const char *name, const char *dest_dir,
                           char *result_name, size_t result_size);
/* Deletion of a folder includes its contents. Requires explicit UI confirmation. */
esp_err_t sd_browser_delete(const char *dir, const char *name);
esp_err_t sd_browser_text_page(const char *dir, const char *name, size_t first_line,
                           char lines[STORAGE_TEXT_ROWS][STORAGE_TEXT_COLS+1],
                           size_t *total_lines, bool *truncated);
/* Read any 256-byte .BIN file, so Verify works outside /EEPROMs too. */
esp_err_t sd_browser_read_eeprom(const char *dir, const char *name, uint8_t bytes[256]);

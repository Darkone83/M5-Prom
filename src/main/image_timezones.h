#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Full 44-byte original Xbox timezone records; based on XboxEepromEditor. */
int image_timezone_count(void);
const char *image_timezone_name(int index);
int image_timezone_find(const uint8_t raw44[44]);
bool image_timezone_apply(uint8_t raw44[44],int index);

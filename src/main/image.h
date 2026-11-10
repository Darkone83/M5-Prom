#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EEPROM_IMAGE_SIZE 256

typedef enum {
    IMAGE_GROUP_IDENTITY=0, IMAGE_GROUP_VIDEO, IMAGE_GROUP_AUDIO,
    IMAGE_GROUP_REGION, IMAGE_GROUP_PARENTAL, IMAGE_GROUP_NETWORK,
    IMAGE_GROUP_COUNT
} image_edit_group_t;

typedef enum {
    IMAGE_EDIT_SERIAL=0, IMAGE_EDIT_MAC,
    IMAGE_EDIT_VIDEO_STD, IMAGE_EDIT_WIDESCREEN, IMAGE_EDIT_LETTERBOX,
    IMAGE_EDIT_480P, IMAGE_EDIT_720P, IMAGE_EDIT_1080I, IMAGE_EDIT_PAL60,
    IMAGE_EDIT_AUDIO_MODE, IMAGE_EDIT_AC3, IMAGE_EDIT_DTS,
    IMAGE_EDIT_GAME_REGION, IMAGE_EDIT_DVD_REGION, IMAGE_EDIT_LANGUAGE, IMAGE_EDIT_TIMEZONE,
    IMAGE_EDIT_GAME_RATING, IMAGE_EDIT_MOVIE_RATING, IMAGE_EDIT_PARENTAL_CODE,
    IMAGE_EDIT_IP, IMAGE_EDIT_DNS, IMAGE_EDIT_GATEWAY, IMAGE_EDIT_SUBNET,
    IMAGE_EDIT_COUNT
} image_edit_field_t;

typedef enum {
    IMAGE_FIX_FACTORY=0, IMAGE_FIX_USER, IMAGE_FIX_VIDEO,
    IMAGE_FIX_LANGUAGE, IMAGE_FIX_DVD, IMAGE_FIX_AUDIO,
    IMAGE_FIX_VIDEO_FLAGS, IMAGE_FIX_GAME_REGION, IMAGE_FIX_TIMEZONE,
    IMAGE_FIX_GAME_RATING, IMAGE_FIX_MOVIE_RATING, IMAGE_FIX_PARENTAL_CODE,
    IMAGE_FIX_COUNT
} image_fix_field_t;

typedef struct {
    bool factory_ok, user_ok, security_ok, serial_ok, mac_ok;
    bool video_ok, video_flags_ok, language_ok, dvd_ok, audio_ok;
    bool game_region_ok, timezone_ok, online_key_ok;
    bool game_rating_ok, movie_rating_ok;
    bool ratings_ok, parental_ok;
    int security_iv; // 0..3, -1 when authentication fails.
} image_report_t;

typedef struct {
    uint8_t original[EEPROM_IMAGE_SIZE]; // Immutable source image.
    uint8_t working[EEPROM_IMAGE_SIZE];  // All edits stay local until explicit HW write.
    bool loaded;
    bool from_chip;
    bool is_donor;
    bool changed;
    bool unsaved;
    char folder[48];
} image_session_t;

void image_begin(image_session_t *session,const uint8_t raw[256],bool from_chip);
void image_report(const image_session_t *session,image_report_t *result);
void image_extract_serial(const uint8_t raw[256],char serial[13]);
/* Cross-reference gate: XbDiag factory/user checksums + picoPromSD
 * successful security HMAC/RC4 authentication. Serial, Ethernet MAC and
 * optional settings are warnings, never standalone write blockers.
 * The SD loader enforces the exact 256-byte image length. */
const char *image_write_block_reason(const image_report_t *report);
enum {
    IMAGE_WRITE_WARN_HMAC = 1u << 0,
    IMAGE_WRITE_WARN_DEBUG = 1u << 1,
    IMAGE_WRITE_WARN_SERIAL = 1u << 2,
    IMAGE_WRITE_WARN_MAC = 1u << 3,
    IMAGE_WRITE_WARN_SETTINGS = 1u << 4,
    IMAGE_WRITE_WARN_ONLINE_KEY = 1u << 5
};
unsigned image_write_warning_flags(const image_report_t *report);
bool image_serial_valid(const uint8_t raw[256]);
uint32_t image_read_u32(const uint8_t raw[256],int off);

const char *image_group_label(image_edit_group_t group);
int image_group_first(image_edit_group_t group);
int image_group_count(image_edit_group_t group);
const char *image_edit_label(image_edit_field_t field);
void image_edit_value(const image_session_t *session,image_edit_field_t field,char *out,size_t cap);
bool image_edit_cycle(image_session_t *session,image_edit_field_t field,int direction);
bool image_edit_text(image_session_t *session,image_edit_field_t field,const char *text);
bool image_edit_is_text(image_edit_field_t field);
int image_edit_text_limit(image_edit_field_t field);
const char *image_edit_text_hint(image_edit_field_t field);

const char *image_fix_label(image_fix_field_t field);
bool image_fix_problem(const image_report_t *report,image_fix_field_t field);
/* Repairs only a diagnosed problem. Healthy fields and unauthenticated
 * security edits return false without changing the session. */
bool image_apply_fix(image_session_t *session,image_fix_field_t field);
void image_revert(image_session_t *session);
void image_refresh_checksums(uint8_t raw[EEPROM_IMAGE_SIZE]);

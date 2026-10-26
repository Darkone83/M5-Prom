/*
 * Original Xbox donor EEPROM generation.
 * Algorithm/field conventions cross-checked with Ernegien/XboxEepromEditor,
 * xemu's hw/xbox/eeprom_generation.c, and Darkone83/XbDiag.
 * This implementation only constructs a *local* raw 256-byte image.
 */
#include "donor.h"
#include "image.h"
#include "image_crypto.h"
#include <string.h>

const char *donor_revision_label(int version)
{
    switch (version) {
    case 1: return "XBOX 1.0";
    case 2: return "XBOX 1.1-1.4";
    case 3: return "XBOX 1.6/1.6B";
    default: return "UNSUPPORTED";
    }
}

static void u32le(uint8_t *p, uint32_t n)
{
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8);
    p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}

bool donor_generate(uint8_t output[256], int version,
                    const uint8_t random_bytes[DONOR_RANDOM_SIZE])
{
    if (!output || !random_bytes || version < 1 || version > 3) return false;
    uint8_t buf[256]={0};
    // Decrypted section: confounder, HDD key, Xbox game region.
    memcpy(buf+0x14,random_bytes,8);
    memcpy(buf+0x1C,random_bytes+8,16);
    u32le(buf+0x2C,1);  // North America. Can be edited in working image.

    // Factory identity. Last '9' is an explicit indicator used by
    // XboxEepromEditor's randomized serial convention, not an original SN.
    for(int i=0;i<11;i++) buf[0x34+i]=(uint8_t)('0'+random_bytes[40+i]%10);
    buf[0x34+11]='9';
    buf[0x40]=0x00; buf[0x41]=0x50; buf[0x42]=0xF2;
    memcpy(buf+0x43,random_bytes+51,3);
    memcpy(buf+0x48,random_bytes+24,16);  // Random online key.
    u32le(buf+0x58,0x00400100u);          // NTSC-M.

    // Full 44-byte London/English default timezone (no host RTC required).
    static const uint8_t london_tz[44]={
        0x00,0x00,0x00,0x00, 0x47,0x4D,0x54,0x00,
        0x42,0x53,0x54,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x0A,0x05,0x00,0x02,
        0x03,0x05,0x00,0x01, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xC4,0xFF,0xFF,0xFF
    };
    memcpy(buf+0x64,london_tz,sizeof(london_tz));
    u32le(buf+0x90,1);   // English.

    image_refresh_checksums(buf);
    image_crypto_encrypt(buf,version);

    // Re-authenticate before allowing the caller to use it.
    image_session_t check_session;
    image_begin(&check_session,buf,false);
    image_report_t report;
    image_report(&check_session,&report);
    const bool ok = report.security_ok && report.security_iv==version &&
                    report.factory_ok && report.user_ok && report.serial_ok &&
                    report.mac_ok && report.video_ok && report.language_ok &&
                    report.audio_ok && report.dvd_ok;
    if(ok) memcpy(output,buf,256);
    memset(buf,0,sizeof(buf));
    memset(&check_session,0,sizeof(check_session));
    return ok;
}

bool donor_copy_image(uint8_t output[256],const uint8_t source[256])
{
    if(!output||!source||output==source)return false;
    memcpy(output,source,256);
    return true;
}

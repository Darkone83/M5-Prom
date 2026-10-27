/* M5 Prom - Xbox HDD key recovery from a validated 256-byte EEPROM image.
 * The original EEPROM image is never changed, and keys are never logged.
 */
#include "hdd_recovery.h"
#include "image_crypto.h"
#include "image.h"
#include <stdio.h>
#include <string.h>

static void wipe(void *ptr, size_t n)
{
    volatile unsigned char *p=(volatile unsigned char *)ptr;
    while(n--) *p++=0;
}

bool hdd_recovery_key_hex(const uint8_t raw[256],char hex[HDD_RECOVERY_KEY_HEX_SIZE])
{
    if(!hex)return false;
    hex[0]=0;
    if(!raw)return false;

    uint8_t decoded[256];
    memcpy(decoded,raw,sizeof(decoded));
    if(image_crypto_decrypt(decoded)<0){
        wipe(decoded,sizeof(decoded));
        return false;
    }
    static const char digits[]="0123456789ABCDEF";
    // In the decrypted security section the 16-byte HDD key is at 0x1c.
    for(unsigned i=0;i<16;i++){
        const uint8_t b=decoded[0x1c+i];
        hex[2*i]=digits[b>>4];
        hex[2*i+1]=digits[b&15];
    }
    hex[32]=0;
    wipe(decoded,sizeof(decoded));
    return true;
}

/* Validate ATA IDENTIFY text without altering byte order or case. Neither
 * the Xbox serial (EEPROM) nor a shortened marketing model is interchangeable
 * with the drive-specific strings returned by the ATA IDENTIFY command.
 */
static bool ata_field_size(const char *str,size_t maximum,size_t *length)
{
    if(!str||!length)return false;
    size_t n=0;
    while(n<=maximum&&str[n]){
        unsigned char c=(unsigned char)str[n];
        if(c<0x20u||c>0x7eu)return false;
        ++n;
    }
    if(n>maximum)return false;
    while(n&&str[n-1]==' ')--n; // ATA trims trailing ASCII spaces ONLY
    if(!n)return false;
    *length=n;
    return true;
}

bool hdd_recovery_ata_fields_valid(const char *model,const char *drive_serial)
{
    size_t ml=0,sl=0;
    return ata_field_size(model,HDD_DRIVE_MODEL_MAX,&ml)&&
           ata_field_size(drive_serial,HDD_DRIVE_SERIAL_MAX,&sl);
}

bool hdd_recovery_ata_password_hex(const uint8_t raw[256],
                                    const char *model,const char *drive_serial,
                                    char hex[HDD_ATA_PASSWORD_HEX_SIZE])
{
    if(!hex)return false;
    hex[0]=0;
    if(!raw)return false;
    size_t ml=0,sl=0;
    if(!ata_field_size(model,HDD_DRIVE_MODEL_MAX,&ml)||
       !ata_field_size(drive_serial,HDD_DRIVE_SERIAL_MAX,&sl))return false;

    uint8_t decoded[256],password[HDD_ATA_PASSWORD_BYTES]={0};
    memcpy(decoded,raw,sizeof(decoded));
    if(image_crypto_decrypt(decoded)<0){wipe(decoded,sizeof(decoded));return false;}
    image_crypto_hmac_sha1_two(decoded+0x1c,16,(const uint8_t*)model,ml,
                               (const uint8_t*)drive_serial,sl,password);
    // Xbox ATA USER password: 20-byte digest + 12 zero bytes (32 total).
    static const char digits[]="0123456789ABCDEF";
    for(size_t i=0;i<sizeof(password);i++){
        hex[i*2]=digits[password[i]>>4];
        hex[i*2+1]=digits[password[i]&15];
    }
    hex[HDD_ATA_PASSWORD_HEX_SIZE-1]=0;
    wipe(decoded,sizeof(decoded));
    wipe(password,sizeof(password));
    return true;
}

bool hdd_recovery_report_for_drive(const uint8_t raw[256],bool from_chip,bool is_donor,
                                   const char *model,const char *drive_serial,
                                   char *out,size_t out_cap)
{
    if(!out||!out_cap)return false;
    out[0]=0;
    if(!raw)return false;
    const bool has_model=model&&*model;
    const bool has_serial=drive_serial&&*drive_serial;
    if(has_model!=has_serial)return false;
    char key[HDD_RECOVERY_KEY_HEX_SIZE]={0};
    char ata[HDD_ATA_PASSWORD_HEX_SIZE]={0};
    if(!hdd_recovery_key_hex(raw,key))return false;
    const bool have_ata=has_model&&has_serial;
    if(have_ata && !hdd_recovery_ata_password_hex(raw,model,drive_serial,ata)){
        wipe(key,sizeof(key));return false;
    }
    char serial[13];image_extract_serial(raw,serial);
    const char *source=is_donor?"Donor working image":
                       from_chip?"Chip read, current working image":"SD image, current working image";
    int n=snprintf(out,out_cap,
        "M5 Prom - Xbox HDD Key Recovery\r\n"
        "================================\r\n"
        "Console serial: %s\r\n"
        "Console MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n"
        "EEPROM source: %s\r\n"
        "EEPROM security HMAC: VALID\r\n"
        "\r\n"
        "Xbox HDD key (16 bytes / 32 hexadecimal digits):\r\n"
        "%s\r\n"
        "\r\n"
        "%s"
        "%s",
        serial,
        raw[0x40],raw[0x41],raw[0x42],raw[0x43],raw[0x44],raw[0x45],
        source,key,
        is_donor?"WARNING: DONOR KEY - may differ from original console.\r\n":"",
        have_ata?"ATA USER PASSWORD GENERATED (32 BYTES):\r\n":
                 "IMPORTANT: This is the Xbox HDD key, NOT an ATA unlock password.\r\n");
    if(n<0||(size_t)n>=out_cap)goto failed;
    if(have_ata){
        const int extra=snprintf(out+n,out_cap-(size_t)n,
            "Drive model (ATA IDENTIFY): %s\r\n"
            "Drive serial (ATA IDENTIFY): %s\r\n"
            "HMAC-SHA1 digest (20 bytes): %.40s\r\n"
            "ATA USER password (32 bytes, hex): %s\r\n"
            "Final 12 password bytes are 00.\r\n"
            "Uses drive model + drive serial exactly, trimming trailing ASCII spaces only.\r\n"
            "No ATA commands were sent. Check actual drive identifiers before use.\r\n",
            model,drive_serial,ata,ata);
        if(extra<0||(size_t)extra>=out_cap-(size_t)n)goto failed;
        n+=extra;
    }else{
        const int extra=snprintf(out+n,out_cap-(size_t)n,
            "The drive-specific ATA password requires the original drive's\r\n"
            "model and serial numbers, as reported by ATA IDENTIFY.\r\n"
            "No ATA user password was generated in this report.\r\n");
        if(extra<0||(size_t)extra>=out_cap-(size_t)n)goto failed;
        n+=extra;
    }
    {
        const int extra=snprintf(out+n,out_cap-(size_t)n,
            "\r\nWARNING: This file contains console-specific recovery secrets.\r\n"
            "Store securely; delete unnecessary copies and avoid public sharing.\r\n");
        if(extra<0||(size_t)extra>=out_cap-(size_t)n)goto failed;
    }
    wipe(key,sizeof(key));wipe(ata,sizeof(ata));
    return true;
failed:
    wipe(key,sizeof(key));wipe(ata,sizeof(ata));wipe(out,out_cap);
    return false;
}

bool hdd_recovery_report(const uint8_t raw[256],bool from_chip,bool is_donor,
                         char *out,size_t out_cap)
{
    return hdd_recovery_report_for_drive(raw,from_chip,is_donor,NULL,NULL,out,out_cap);
}

/* Offline Xbox EEPROM image editor. Field definitions/checksums from XbDiag,
 * timezone data from Ernegien/XboxEepromEditor. No kernel calls, I2C, or SD.
 */
#include "image.h"
#include "image_crypto.h"
#include "image_timezones.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

uint32_t image_read_u32(const uint8_t raw[256],int off)
{
    return (uint32_t)raw[off]|((uint32_t)raw[off+1]<<8)|
           ((uint32_t)raw[off+2]<<16)|((uint32_t)raw[off+3]<<24);
}
static void write_u32(uint8_t raw[256],int off,uint32_t v)
{
    raw[off]=(uint8_t)v;raw[off+1]=(uint8_t)(v>>8);
    raw[off+2]=(uint8_t)(v>>16);raw[off+3]=(uint8_t)(v>>24);
}
static uint32_t checksum(const uint8_t *data,int off,int size)
{
    uint32_t lo=0,hi=0;
    for(int i=0;i<size;i+=4){
        uint32_t n=image_read_u32(data,off+i);
        uint32_t next=lo+n;
        if(next<lo)hi++;
        lo=next;
    }
    return lo+hi;
}
static void checksum_update(uint8_t raw[256],bool factory)
{
    if(factory)write_u32(raw,0x30,~checksum(raw,0x34,0x2C));
    else write_u32(raw,0x60,~checksum(raw,0x64,0x5C));
}
void image_refresh_checksums(uint8_t raw[256])
{
    if(raw){checksum_update(raw,true);checksum_update(raw,false);}
}
bool image_serial_valid(const uint8_t raw[256])
{
    bool different=false;
    for(int i=0;i<12;i++){
        if(raw[0x34+i]<'0'||raw[0x34+i]>'9')return false;
        if(i&&raw[0x34+i]!=raw[0x34])different=true;
    }
    return different;
}
void image_extract_serial(const uint8_t raw[256],char serial[13])
{
    if(image_serial_valid(raw)){memcpy(serial,raw+0x34,12);serial[12]=0;}
    else strcpy(serial,"UNKNOWN");
}
static bool mac_valid(const uint8_t raw[256])
{
    if(raw[0x40]&1u)return false;
    bool any=false,all_ff=true;
    for(int i=0;i<6;i++){
        if(raw[0x40+i])any=true;
        if(raw[0x40+i]!=0xff)all_ff=false;
    }
    return any&&!all_ff;
}
static void mark_changed(image_session_t *s,const uint8_t before[256])
{
    if(memcmp(before,s->working,256)!=0)s->unsaved=true;
    s->changed=memcmp(s->original,s->working,256)!=0;
}
void image_begin(image_session_t *s,const uint8_t raw[256],bool from_chip)
{
    if(!s||!raw)return;
    memset(s,0,sizeof(*s));
    memcpy(s->original,raw,256);memcpy(s->working,raw,256);
    s->loaded=true;s->from_chip=from_chip;
    image_extract_serial(raw,s->folder);
}
static bool parental_valid(uint32_t code)
{
    for(int i=0;i<8;i++)if(((code>>(i*4))&15u)>4)return false;
    return true;
}
static const uint32_t movie_options[]={0,1,2,4,5,7};
static bool movie_valid(uint32_t movie)
{
    for(size_t i=0;i<sizeof(movie_options)/sizeof(movie_options[0]);i++)
        if(movie==movie_options[i])return true;
    return false;
}
void image_report(const image_session_t *s,image_report_t *r)
{
    if(!r)return;
    memset(r,0,sizeof(*r));r->security_iv=-1;
    if(!s||!s->loaded)return;
    const uint8_t *d=s->working;
    r->factory_ok=checksum(d,0x30,0x30)==0xffffffffu;
    r->user_ok=checksum(d,0x60,0x60)==0xffffffffu;
    r->serial_ok=image_serial_valid(d);
    r->mac_ok=mac_valid(d);
    uint32_t vs=image_read_u32(d,0x58);
    r->video_ok=(vs==0x00400100u||vs==0x00400200u||vs==0x00800300u||vs==0x00400400u);
    r->video_flags_ok=(image_read_u32(d,0x94)&~0x005F0000u)==0;
    r->language_ok=image_read_u32(d,0x90)<=9;
    r->dvd_ok=image_read_u32(d,0xBC)<=6;
    uint32_t audio=image_read_u32(d,0x98);
    r->audio_ok=(audio&~0x00030003u)==0&&(audio&3u)<=2;
    uint32_t game=image_read_u32(d,0x9C);
    r->ratings_ok=(game<=8||game==0xff)&&movie_valid(image_read_u32(d,0xA4));
    r->parental_ok=parental_valid(image_read_u32(d,0xA0));
    uint8_t tmp[256];memcpy(tmp,d,256);
    r->security_iv=image_crypto_decrypt(tmp);
    r->security_ok=r->security_iv>=0;
    memset(tmp,0,sizeof(tmp));
}
static const char *group_names[IMAGE_GROUP_COUNT]={
    "IDENTITY","VIDEO","AUDIO","REGION","PARENTAL","NETWORK"
};
static const int group_starts[IMAGE_GROUP_COUNT+1]={
    IMAGE_EDIT_SERIAL,IMAGE_EDIT_VIDEO_STD,IMAGE_EDIT_AUDIO_MODE,
    IMAGE_EDIT_GAME_REGION,IMAGE_EDIT_GAME_RATING,IMAGE_EDIT_IP,IMAGE_EDIT_COUNT
};
const char *image_group_label(image_edit_group_t g)
{return g>=0&&g<IMAGE_GROUP_COUNT?group_names[g]:"UNKNOWN";}
int image_group_first(image_edit_group_t g)
{return g>=0&&g<IMAGE_GROUP_COUNT?group_starts[g]:0;}
int image_group_count(image_edit_group_t g)
{return g>=0&&g<IMAGE_GROUP_COUNT?group_starts[g+1]-group_starts[g]:0;}
static const char *field_names[IMAGE_EDIT_COUNT]={
    "SERIAL NUMBER","MAC ADDRESS",
    "VIDEO STANDARD","WIDESCREEN","LETTERBOX","480P","720P","1080I","PAL60",
    "SPEAKER MODE","DOLBY AC3","DTS",
    "GAME REGION","DVD REGION","LANGUAGE","TIMEZONE",
    "GAME RATING","MOVIE RATING","PARENTAL CODE",
    "STATIC IP","DNS SERVER","GATEWAY","SUBNET MASK"
};
const char *image_edit_label(image_edit_field_t field)
{return field>=0&&field<IMAGE_EDIT_COUNT?field_names[field]:"UNKNOWN";}
static const uint32_t standards[]={0x00400100u,0x00400200u,0x00800300u,0x00400400u};
static const char *standard_labels[]={"NTSC-M","NTSC-J","PAL-I","PAL-M"};
static const uint32_t regions[]={1u,2u,4u,0x80000000u};
static const char *region_labels[]={"N. AMERICA","JAPAN","REST OF WORLD","MANUFACTURING"};
static const char *languages[]={"NEUTRAL","ENGLISH","JAPANESE","GERMAN","FRENCH","SPANISH","ITALIAN","KOREAN","CHINESE","PORTUGUESE"};
static const uint32_t game_levels[]={0,1,2,3,4,5,6,7,8,0xff};
static const char *game_labels[]={"OFF","AO","M","T","E","K-A","EC","LEVEL 7","LEVEL 8","ALL BLOCKED"};
static const char *movie_labels[]={"OFF/NR","NC17","R","PG13","PG","G"};
static const char *audio_modes[]={"STEREO","MONO","SURROUND"};
static int option_index(uint32_t v,const uint32_t *items,int n)
{for(int i=0;i<n;i++)if(items[i]==v)return i;return -1;}
static int rotate_index(int cur,int count,int direction)
{
    if(cur<0)cur=0;
    return (cur+(direction<0?count-1:1))%count;
}
static uint32_t flag_for(image_edit_field_t f)
{
    switch(f){
        case IMAGE_EDIT_WIDESCREEN:return 0x00010000u;
        case IMAGE_EDIT_LETTERBOX:return 0x00100000u;
        case IMAGE_EDIT_480P:return 0x00080000u;
        case IMAGE_EDIT_720P:return 0x00020000u;
        case IMAGE_EDIT_1080I:return 0x00040000u;
        case IMAGE_EDIT_PAL60:return 0x00400000u;
        case IMAGE_EDIT_AC3:return 0x00010000u;
        case IMAGE_EDIT_DTS:return 0x00020000u;
        default:return 0;
    }
}
static int ipv4_offset(image_edit_field_t f)
{
    switch(f){
        case IMAGE_EDIT_IP:return 0xA8;
        case IMAGE_EDIT_DNS:return 0xAC;
        case IMAGE_EDIT_GATEWAY:return 0xB0;
        case IMAGE_EDIT_SUBNET:return 0xB4;
        default:return -1;
    }
}
bool image_edit_is_text(image_edit_field_t f)
{return f==IMAGE_EDIT_SERIAL||f==IMAGE_EDIT_MAC||f==IMAGE_EDIT_PARENTAL_CODE||ipv4_offset(f)>=0;}
int image_edit_text_limit(image_edit_field_t f)
{
    if(f==IMAGE_EDIT_SERIAL||f==IMAGE_EDIT_MAC)return 12;
    if(f==IMAGE_EDIT_PARENTAL_CODE)return 8;
    return ipv4_offset(f)>=0?15:0;
}
const char *image_edit_text_hint(image_edit_field_t f)
{
    if(f==IMAGE_EDIT_SERIAL)return "12 DECIMAL DIGITS";
    if(f==IMAGE_EDIT_MAC)return "12 HEX DIGITS";
    if(f==IMAGE_EDIT_PARENTAL_CODE)return "8 DIGITS (0-4), 00000000=OFF";
    return "IPV4: X.X.X.X";
}
void image_edit_value(const image_session_t *s,image_edit_field_t f,char *out,size_t cap)
{
    if(!out||cap==0)return;
    *out=0;
    if(!s||!s->loaded)return;
    const uint8_t *d=s->working;
    int idx;
    if(f==IMAGE_EDIT_SERIAL){char serial[13];image_extract_serial(d,serial);snprintf(out,cap,"%s",serial);return;}
    if(f==IMAGE_EDIT_MAC){snprintf(out,cap,"%02X%02X%02X%02X%02X%02X",d[0x40],d[0x41],d[0x42],d[0x43],d[0x44],d[0x45]);return;}
    int ipoff=ipv4_offset(f);
    if(ipoff>=0){snprintf(out,cap,"%u.%u.%u.%u",d[ipoff],d[ipoff+1],d[ipoff+2],d[ipoff+3]);return;}
    if(f==IMAGE_EDIT_PARENTAL_CODE){
        uint32_t p=image_read_u32(d,0xA0);
        if(p)snprintf(out,cap,"%08lX",(unsigned long)p);
        else snprintf(out,cap,"DISABLED");
        return;
    }
    switch(f){
        case IMAGE_EDIT_VIDEO_STD:
            idx=option_index(image_read_u32(d,0x58),standards,4);
            snprintf(out,cap,"%s",idx<0?"INVALID":standard_labels[idx]);break;
        case IMAGE_EDIT_GAME_REGION:{
            uint8_t dec[256];memcpy(dec,d,256);
            if(image_crypto_decrypt(dec)<0)snprintf(out,cap,"SECURITY INVALID");
            else {
                idx=option_index(image_read_u32(dec,0x2C),regions,4);
                snprintf(out,cap,"%s",idx<0?"CUSTOM":region_labels[idx]);
            }
            memset(dec,0,sizeof(dec));break;
        }
        case IMAGE_EDIT_LANGUAGE:{
            uint32_t lang=image_read_u32(d,0x90);
            snprintf(out,cap,"%s",lang<10?languages[lang]:"INVALID");break;
        }
        case IMAGE_EDIT_DVD_REGION:{
            uint32_t z=image_read_u32(d,0xBC);
            if(z==0)snprintf(out,cap,"FREE");
            else if(z<=6)snprintf(out,cap,"REGION %lu",(unsigned long)z);
            else snprintf(out,cap,"INVALID");
            break;
        }
        case IMAGE_EDIT_TIMEZONE:
            idx=image_timezone_find(d+0x64);
            snprintf(out,cap,"%s",image_timezone_name(idx));break;
        case IMAGE_EDIT_AUDIO_MODE:{
            uint32_t mode=image_read_u32(d,0x98)&3u;
            snprintf(out,cap,"%s",mode<3?audio_modes[mode]:"INVALID");break;
        }
        case IMAGE_EDIT_GAME_RATING:{
            idx=option_index(image_read_u32(d,0x9C),game_levels,10);
            snprintf(out,cap,"%s",idx>=0?game_labels[idx]:"CUSTOM");break;
        }
        case IMAGE_EDIT_MOVIE_RATING:{
            idx=option_index(image_read_u32(d,0xA4),movie_options,6);
            snprintf(out,cap,"%s",idx>=0?movie_labels[idx]:"CUSTOM");break;
        }
        default:{
            uint32_t flag=flag_for(f);
            if(flag)snprintf(out,cap,"%s",(image_read_u32(d,(f==IMAGE_EDIT_AC3||f==IMAGE_EDIT_DTS)?0x98:0x94)&flag)?"ON":"OFF");
        }
    }
}
bool image_edit_cycle(image_session_t *s,image_edit_field_t f,int dir)
{
    if(!s||!s->loaded||!dir||image_edit_is_text(f))return false;
    uint8_t before[256];memcpy(before,s->working,256);
    uint8_t *d=s->working;
    switch(f){
        case IMAGE_EDIT_VIDEO_STD:{
            int i=option_index(image_read_u32(d,0x58),standards,4);
            write_u32(d,0x58,standards[rotate_index(i,4,dir)]);checksum_update(d,true);break;
        }
        case IMAGE_EDIT_GAME_REGION:{
            uint8_t dec[256];memcpy(dec,d,256);
            int ver=image_crypto_decrypt(dec);
            if(ver<0){memset(dec,0,sizeof(dec));return false;}
            int i=option_index(image_read_u32(dec,0x2C),regions,4);
            write_u32(dec,0x2C,regions[rotate_index(i,4,dir)]);
            image_crypto_encrypt(dec,ver);
            memcpy(d,dec,0x30);
            memset(dec,0,sizeof(dec));break;
        }
        case IMAGE_EDIT_LANGUAGE:{
            uint32_t v=image_read_u32(d,0x90);
            write_u32(d,0x90,(uint32_t)rotate_index(v<10?(int)v:-1,10,dir));
            checksum_update(d,false);break;
        }
        case IMAGE_EDIT_DVD_REGION:{
            uint32_t v=image_read_u32(d,0xBC);
            write_u32(d,0xBC,(uint32_t)rotate_index(v<=6?(int)v:-1,7,dir));
            checksum_update(d,false);break;
        }
        case IMAGE_EDIT_TIMEZONE:{
            int i=image_timezone_find(d+0x64);
            int n=image_timezone_count();
            if(!image_timezone_apply(d+0x64,rotate_index(i,n,dir)))return false;
            checksum_update(d,false);break;
        }
        case IMAGE_EDIT_AUDIO_MODE:{
            uint32_t v=image_read_u32(d,0x98);
            write_u32(d,0x98,(v&~3u)|(uint32_t)rotate_index((v&3u)<=2?(int)(v&3u):-1,3,dir));
            checksum_update(d,false);break;
        }
        case IMAGE_EDIT_GAME_RATING:{
            int i=option_index(image_read_u32(d,0x9C),game_levels,10);
            write_u32(d,0x9C,game_levels[rotate_index(i,10,dir)]);checksum_update(d,false);break;
        }
        case IMAGE_EDIT_MOVIE_RATING:{
            int i=option_index(image_read_u32(d,0xA4),movie_options,6);
            write_u32(d,0xA4,movie_options[rotate_index(i,6,dir)]);checksum_update(d,false);break;
        }
        default:{
            uint32_t flag=flag_for(f);
            if(!flag)return false;
            int off=(f==IMAGE_EDIT_AC3||f==IMAGE_EDIT_DTS)?0x98:0x94;
            write_u32(d,off,image_read_u32(d,off)^flag);
            checksum_update(d,false);
        }
    }
    mark_changed(s,before);
    return memcmp(before,d,256)!=0;
}
static int hex_digit(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static bool parse_ipv4(const char *text,uint8_t value[4])
{
    int slot=0,number=0,digits=0;
    if(!text||!*text)return false;
    for(const char *p=text;*p;p++){
        if(*p=='.'){
            if(slot>=3||digits==0)return false;
            value[slot++]=(uint8_t)number;number=0;digits=0;
        }else if(*p>='0'&&*p<='9'){
            if(++digits>3)return false;
            number=number*10+(*p-'0');if(number>255)return false;
        }else return false;
    }
    if(slot!=3||digits==0)return false;
    value[3]=(uint8_t)number;return true;
}
bool image_edit_text(image_session_t *s,image_edit_field_t f,const char *text)
{
    if(!s||!s->loaded||!text)return false;
    uint8_t before[256];memcpy(before,s->working,256);
    if(f==IMAGE_EDIT_SERIAL){
        if(strlen(text)!=12)return false;
        bool different=false;
        for(int i=0;i<12;i++){
            if(text[i]<'0'||text[i]>'9')return false;
            if(i&&text[i]!=text[0])different=true;
        }
        if(!different)return false;
        memcpy(s->working+0x34,text,12);checksum_update(s->working,true);
    }else if(f==IMAGE_EDIT_MAC){
        if(strlen(text)!=12)return false;
        uint8_t mac[6];
        for(int i=0;i<6;i++){
            int a=hex_digit(text[i*2]),b=hex_digit(text[i*2+1]);
            if(a<0||b<0)return false;
            mac[i]=(uint8_t)((a<<4)|b);
        }
        if(mac[0]&1u)return false;
        bool any=false;for(int i=0;i<6;i++)if(mac[i])any=true;
        if(!any)return false;
        memcpy(s->working+0x40,mac,6);checksum_update(s->working,true);
    }else if(f==IMAGE_EDIT_PARENTAL_CODE){
        if(strlen(text)!=8)return false;
        uint32_t code=0;
        for(int i=0;i<8;i++){
            int d=hex_digit(text[i]);
            if(d<0||d>4)return false;
            code=(code<<4)|(uint32_t)d;
        }
        write_u32(s->working,0xA0,code);checksum_update(s->working,false);
    }else{
        int off=ipv4_offset(f);
        uint8_t ipv4[4];
        if(off<0||!parse_ipv4(text,ipv4))return false;
        memcpy(s->working+off,ipv4,4);checksum_update(s->working,false);
    }
    mark_changed(s,before);
    return true;
}
const char *image_fix_label(image_fix_field_t f)
{
    static const char *labels[]={"FACTORY CHECKSUM","USER CHECKSUM","VIDEO STANDARD","LANGUAGE","DVD REGION","AUDIO FLAGS"};
    return f>=0&&f<IMAGE_FIX_COUNT?labels[f]:"UNKNOWN";
}
bool image_fix_problem(const image_report_t *r,image_fix_field_t f)
{
    if(!r)return false;
    switch(f){
        case IMAGE_FIX_FACTORY:return !r->factory_ok;
        case IMAGE_FIX_USER:return !r->user_ok;
        case IMAGE_FIX_VIDEO:return !r->video_ok;
        case IMAGE_FIX_LANGUAGE:return !r->language_ok;
        case IMAGE_FIX_DVD:return !r->dvd_ok;
        case IMAGE_FIX_AUDIO:return !r->audio_ok;
        default:return false;
    }
}
bool image_apply_fix(image_session_t *s,image_fix_field_t f)
{
    if(!s||!s->loaded)return false;
    uint8_t before[256];memcpy(before,s->working,256);
    uint8_t *d=s->working;
    switch(f){
        case IMAGE_FIX_FACTORY:checksum_update(d,true);break;
        case IMAGE_FIX_USER:checksum_update(d,false);break;
        case IMAGE_FIX_VIDEO:write_u32(d,0x58,0x00400100u);checksum_update(d,true);break;
        case IMAGE_FIX_LANGUAGE:write_u32(d,0x90,1);checksum_update(d,false);break;
        case IMAGE_FIX_DVD:write_u32(d,0xBC,0);checksum_update(d,false);break;
        case IMAGE_FIX_AUDIO:{
            uint32_t mode=image_read_u32(d,0x98)&0x00030003u;
            if((mode&3u)==3)mode&=~3u;
            write_u32(d,0x98,mode);checksum_update(d,false);break;
        }
        default:return false;
    }
    mark_changed(s,before);
    return memcmp(before,s->working,256)!=0;
}
void image_revert(image_session_t *s)
{
    if(!s||!s->loaded)return;
    memcpy(s->working,s->original,256);
    s->changed=false;s->unsaved=false;
}

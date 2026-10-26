/*
 * M5 Prom v0.9.3 — Xbox EEPROM Suite
 * Read physical EEPROM -> preserve original on SD -> work on an offline copy.
 * Write only after EEPROM detection, repeated reads, verified SD backup,
 * explicit confirmation, then byte-write/verify. Supports donor generation.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "display.h"
#include "keyboard.h"
#include "eeprom.h"
#include "storage.h"
#include "image.h"
#include "donor.h"
#include "sound.h"
#include "battery.h"
#include "settings.h"
#include "wifi_manager.h"
#include "web_files.h"
#include "operation_guard.h"
#include "esp_random.h"
#include "bootloader_random.h"

static const char *TAG="m5_prom";
typedef enum {
    PAGE_MENU=0,PAGE_RESULT,PAGE_INSPECT,PAGE_EDIT,PAGE_REPAIR,
    PAGE_TEXT_EDIT,PAGE_CONFIRM_FIX,PAGE_CONFIRM_DISCARD,
    PAGE_FOLDERS,PAGE_FILES,PAGE_PINS,PAGE_DONOR_MODE,PAGE_DONOR,PAGE_CONFIRM_WRITE,PAGE_BATTERY,
    PAGE_SETTINGS,PAGE_WIFI_SCAN,PAGE_WIFI_PASSWORD,PAGE_WIFI_STATUS,PAGE_DIAGNOSTICS
} page_t;
typedef enum {PENDING_NONE=0,PENDING_READ,PENDING_LOAD,PENDING_DONOR} pending_t;
typedef enum {BROWSER_OPEN=0,BROWSER_DONOR_SOURCE} browser_purpose_t;
static page_t page=PAGE_MENU;
static pending_t pending=PENDING_NONE;
static browser_purpose_t browser_purpose=BROWSER_OPEN;
static image_session_t session;
static image_fix_field_t pending_fix;
static image_edit_field_t text_field;
static bool sd_ok=false,i2c_ok=false,battery_ok=false;
static bool eeprom_checked=false,eeprom_detected=false;
static esp_err_t eeprom_probe_error=ESP_ERR_INVALID_STATE;
static bool write_guard_held=false;
static int menu_selected=0,inspect_selected=0,edit_selected=0,repair_selected=0;
static image_edit_group_t edit_group=IMAGE_GROUP_IDENTITY;
static bool edit_notice=false,entry_error=false;
static int list_selected=0;
static size_t folder_count=0,file_count=0;
static char folder_list[STORAGE_MAX_ENTRIES][STORAGE_NAME_SIZE];
static char file_list[STORAGE_MAX_ENTRIES][STORAGE_NAME_SIZE];
static char selected_folder[STORAGE_NAME_SIZE];
static char entry_text[24];
static size_t entry_len=0;
static char lines[5][38];
static bool result_good=false;
static int donor_selected=0,donor_mode_selected=0;
static int settings_selected=0,wifi_selected=0;
static bool wifi_ok=false,wifi_password_visible=false,wifi_password_error=false;
static m5_wifi_scan_entry_t wifi_networks[12];
static size_t wifi_network_count=0;
static char wifi_selected_ssid[33],wifi_password[65];
static size_t wifi_password_len=0;
#define SETTINGS_COUNT 9
static uint8_t prewrite_target[256];
static bool prewrite_valid=false;
static bool write_armed=false;
static uint16_t write_diffs=0;
static bool write_security_changes=false;
static bool write_revision_mismatch=false;
static char write_target_folder[STORAGE_NAME_SIZE];
static char write_backup_name[STORAGE_NAME_SIZE];

#define WIDTH 240
#define LIST_FIRST_Y 46
#define LIST_STEP 14
#define LIST_VISIBLE 5
#define MENU_COUNT 9
#define MENU_COLS 3

// A nine-command workspace, rather than a long scrolling startup list.
static const char *menu_labels[MENU_COUNT]={
    "READ", "SAVE", "LOAD", "INSPECT", "EDIT", "REPAIR",
    "WRITE", "REVERT", "DONOR"
};
static const char *menu_tips[MENU_COUNT]={
    "READ TWICE / BACK UP ORIGINAL",
    "SAVE WORKING IMAGE TO MICROSD",
    "LOAD A BACKUP FROM MICROSD",
    "VIEW SERIAL / MAC / CHECKSUMS",
    "CHANGE FIELDS IN MEMORY ONLY",
    "CHECK / REPAIR WORKING IMAGE",
    "DETECT / BACKUP / WRITE / VERIFY",
    "RESTORE ORIGINAL WORKING BYTES",
    "CREATE NEW EEPROM FOR RECOVERY / TEST"
};
// Small handcrafted monochrome UI glyphs. Each byte is an 8-pixel row.
// This has no dependency on image assets, PSRAM, or font libraries.
static const uint8_t icon_bits[MENU_COUNT][8]={
    {0x3C,0x42,0xA5,0x81,0x99,0x81,0x42,0x3C}, // Read: chip
    {0x7E,0x42,0x5A,0x42,0x7E,0x66,0x66,0x7E}, // Save: disk
    {0x00,0x70,0x88,0xFE,0x82,0x82,0xFE,0x00}, // Load: folder
    {0x3C,0x42,0x42,0x3C,0x18,0x24,0x42,0x80}, // Inspect: lens
    {0x02,0x06,0x0E,0x1C,0x38,0x70,0x60,0x00}, // Edit: pen
    {0x42,0x24,0x18,0x3C,0x18,0x24,0x42,0x00}, // Repair: tool
    {0x18,0x24,0x24,0x7E,0x42,0x5A,0x7E,0x00}, // Write: chip program
    {0x3E,0x40,0x80,0x9C,0x82,0x42,0x3C,0x00}, // Revert: return
    {0x24,0x24,0x7E,0x42,0x42,0x7E,0x24,0x24}, // Donor: new image
};
static void icon8(int x,int y,int which,uint32_t color,int scale)
{
    if(which<0||which>=MENU_COUNT)return;
    for(int row=0;row<8;row++){
        uint8_t bits=icon_bits[which][row];
        for(int col=0;col<8;col++)
            if(bits&(uint8_t)(0x80u>>col))
                display_rect(x+col*scale,y+row*scale,scale,scale,color);
    }
}

// Crop long file names without making text spill into adjacent columns.
// The full file name remains in the SD filesystem and working session.
static void fit_text(int x,int y,const char *text,int max_chars,uint32_t color,int scale)
{
    if(!text||max_chars<=0)return;
    if(max_chars>45)max_chars=45;
    char tmp[48];
    size_t n=strlen(text);
    size_t cap=(size_t)max_chars;
    if(n<=cap){
        memcpy(tmp,text,n+1);
    }else if(cap>=3){
        memcpy(tmp,text,cap-2);
        tmp[cap-2]='.';
        tmp[cap-1]='.';
        tmp[cap]=0;
    }else{
        memcpy(tmp,text,cap);
        tmp[cap]=0;
    }
    display_text(x,y,tmp,color,scale);
}
static void show_message(bool good,const char *title,const char *a,const char *b,const char *c)
{
    page=PAGE_RESULT;result_good=good;
    sound_notify(good); /* queued: never blocks the display/input task */
    snprintf(lines[0],sizeof(lines[0]),"%s",title?title:"");
    snprintf(lines[1],sizeof(lines[1]),"%s",a?a:"");
    snprintf(lines[2],sizeof(lines[2]),"%s",b?b:"");
    snprintf(lines[3],sizeof(lines[3]),"%s",c?c:"");
}
/* A tiny calibrated-voltage SoC badge, always visible. The trailing sign
 * indicates voltage trend, NOT confirmed charge/discharge current.
 */
static void battery_percent_text(battery_status_t b,char out[5])
{
    if(!b.valid){memcpy(out,"--%",4);return;}
    int n=b.percentage;
    if(n<0)n=0;
    if(n>100)n=100;
    if(n==100){memcpy(out,"100%",5);return;}
    if(n>=10){out[0]=(char)('0'+n/10);out[1]=(char)('0'+n%10);out[2]='%';out[3]=0;}
    else {out[0]=(char)('0'+n);out[1]='%';out[2]=0;}
}
static uint32_t battery_color(battery_status_t b)
{
    if(!b.valid)return UI_MUTED;
    if(b.percentage<=15)return UI_BAD;
    if(b.percentage<=30)return UI_WARN;
    return UI_GOOD;
}
static void battery_badge(void)
{
    const battery_status_t b=battery_get_status();
    const uint32_t color=battery_color(b);
    const int x=174,y=9;
    display_frame(x,y,15,9,color);
    display_rect(x+15,y+3,2,3,color);
    if(b.valid){
        int fill=b.percentage*11/100;
        if(b.percentage>0&&fill==0)fill=1;
        if(fill>0)display_rect(x+2,y+2,fill,5,color);
    }
    char pct[5];
    battery_percent_text(b,pct);
    display_text(194,y+1,pct,color,1);
    const char *trend=b.trend==BATTERY_TREND_RISING?"+":
                      b.trend==BATTERY_TREND_FALLING?"-":
                      b.trend==BATTERY_TREND_STABLE?"=":"?";
    display_text(225,y+1,trend,b.trend==BATTERY_TREND_UNKNOWN?UI_MUTED:UI_CYAN,1);
}
static const char *battery_trend_name(battery_trend_t t)
{
    switch(t){
        case BATTERY_TREND_RISING:return "RISING";
        case BATTERY_TREND_FALLING:return "FALLING";
        case BATTERY_TREND_STABLE:return "STABLE";
        default:return "ANALYZING";
    }
}

static void top_rail(void)
{
    display_rect(0,0,183,2,UI_ACCENT);
    display_rect(183,0,57,2,UI_CYAN);
}
static void header(const char *name,const char *subtitle)
{
    display_clear(UI_BG);
    display_rect(0,2,WIDTH,40,UI_HEADER);
    top_rail();
    if(name && strlen(name)<=13)fit_text(9,8,name,13,UI_WHITE,2);
    else fit_text(9,12,name,26,UI_WHITE,1);
    battery_badge();
    display_rect(8,29,224,12,UI_PANEL);
    display_rect(8,29,2,12,UI_ACCENT_DIM);
    fit_text(14,32,subtitle?subtitle:"LOCAL EEPROM WORKSPACE",34,UI_MUTED,1);
}
static void footer(const char *help)
{
    display_rect(0,119,WIDTH,16,UI_HEADER);
    display_rect(0,119,WIDTH,1,UI_LINE);
    fit_text(8,124,help,37,UI_MUTED,1);
}
static void footer_indexed(const char *help,int selected,int count)
{
    footer(help);
    if(count>0){
        // Page indicator is always two decimal digits: source lists max at 40.
        int here=selected+1;
        int total=count;
        if(here<0)here=0;
        if(here>99)here=99;
        if(total>99)total=99;
        char index[6]={
            (char)('0'+here/10),(char)('0'+here%10),'/',
            (char)('0'+total/10),(char)('0'+total%10),0
        };
        display_rect(198,121,38,12,UI_PANEL);
        display_text(202,124,index,UI_ACCENT,1);
    }
}
/* Direct updates during a synchronous operation. Keyboard handling is idle
 * until the operation completes; there is no cancel path mid-chip-write.
 * Do not render a fictional time-based percentage: each percentage corresponds
 * to completed I2C reads, ACKed EEPROM bytes, or verified FAT-file stages.
 */
static int progress_last_percent=-1;
static char progress_last_phase[32];
static void progress_reset(void)
{
    progress_last_percent=-1;
    progress_last_phase[0]=0;
}
static void progress_draw(const char *title,const char *phase,unsigned percent,
                          unsigned done,unsigned total,const char *note)
{
    if(percent>100)percent=100;
    if(progress_last_percent>=0 && !strcmp(progress_last_phase,phase) &&
       percent<100 && percent<(unsigned)(progress_last_percent+4))return;
    progress_last_percent=(int)percent;
    snprintf(progress_last_phase,sizeof(progress_last_phase),"%s",phase);
    header(title,"WORKING / DO NOT DISCONNECT");
    fit_text(12,52,phase,35,UI_WHITE,1);
    display_rect(10,70,220,18,UI_PANEL);
    display_frame(10,70,220,18,UI_LINE);
    if(percent>0)display_rect(12,72,(int)(216u*percent/100u),14,UI_ACCENT);
    char pct[8];snprintf(pct,sizeof(pct),"%3u%%",percent);
    display_text(199,92,pct,UI_CYAN,1);
    if(total){
        char counter[36];
        snprintf(counter,sizeof(counter),"%u / %u CONFIRMED",done,total);
        fit_text(12,94,counter,30,UI_MUTED,1);
    }else fit_text(12,94,note?note:"",30,UI_MUTED,1);
    footer("KEEP USB / XBOX / SD CONNECTED");
    display_present();
}
typedef enum {OP_PROGRESS_READ=0,OP_PROGRESS_PREWRITE,OP_PROGRESS_COMMIT} progress_job_t;
static void on_eeprom_progress(void *ctx,eeprom_progress_phase_t phase,
                               unsigned completed,unsigned total)
{
    progress_job_t job=*(const progress_job_t *)ctx;
    const char *title=job==OP_PROGRESS_READ?"READ EEPROM":
                      job==OP_PROGRESS_PREWRITE?"PREPARE WRITE":"WRITE EEPROM";
    const char *step="PROBING EEPROM";
    unsigned percent=0;
    if(job==OP_PROGRESS_COMMIT){
        switch(phase){
            case EEPROM_PROGRESS_PROBE: step="CHECKING TARGET";percent=2;break;
            case EEPROM_PROGRESS_READ_FIRST:step="RECHECK / PASS 1";percent=5;break;
            case EEPROM_PROGRESS_READ_SECOND:step="RECHECK / PASS 2";percent=9;break;
            case EEPROM_PROGRESS_READ_MATCH:step="MATCHING PREWRITE COPY";percent=12;break;
            case EEPROM_PROGRESS_PROGRAM:
                step="PROGRAMMING / ACKED BYTES";
                percent=15u+(total?75u*completed/total:0);break;
            case EEPROM_PROGRESS_VERIFY_FIRST:step="VERIFY / READ 1";percent=94;break;
            case EEPROM_PROGRESS_VERIFY_SECOND:step="VERIFY / READ 2";percent=98;break;
            case EEPROM_PROGRESS_VERIFIED:step="FULL READBACK VERIFIED";percent=100;break;
        }
    }else{
        unsigned top=job==OP_PROGRESS_PREWRITE?42:75;
        switch(phase){
            case EEPROM_PROGRESS_PROBE:step="DETECTING EEPROM 0X54";percent=3;break;
            case EEPROM_PROGRESS_READ_FIRST:step="CHIP READ / PASS 1";percent=top/2;break;
            case EEPROM_PROGRESS_READ_SECOND:step="CHIP READ / PASS 2";percent=top-3;break;
            case EEPROM_PROGRESS_READ_MATCH:step="TWO READS MATCHED";percent=top;break;
            default:return;
        }
    }
    progress_draw(title,step,percent,
                  phase==EEPROM_PROGRESS_PROGRAM?completed:0,
                  phase==EEPROM_PROGRESS_PROGRAM?total:0,"VERIFIED PHYSICAL TRANSACTIONS");
}
static bool acquire_operation_or_warn(void)
{
    if(operation_guard_try_acquire())return true;
    show_message(false,"STORAGE / WEB BUSY","ANOTHER SD OPERATION IS ACTIVE",
                 "FINISH WEB UPLOAD / RETRY","NO DATA WAS MODIFIED");
    return false;
}
static void list_item(int y,const char *name,bool selected,const char *right,uint32_t color)
{
    int x=20,max_chars=34;
    if(selected){
        display_rect(7,y-3,226,14,UI_FOCUS);
        display_rect(7,y-3,2,14,UI_ACCENT);
        display_rect(13,y,2,6,UI_ACCENT);
    }else{
        display_rect(7,y+10,226,1,UI_PANEL);
    }
    if(right){
        int rlen=(int)strlen(right);
        if(rlen>15)rlen=15;
        int rx=231-rlen*6;
        max_chars=(rx-x-8)/6;
        if(max_chars<3)max_chars=3;
        fit_text(rx,y,right,15,color,1);
    }
    fit_text(x,y,name,max_chars,selected?UI_WHITE:UI_MUTED,1);
}
static int list_start(int selected,int total)
{
    if(total<=LIST_VISIBLE)return 0;
    int start=selected-LIST_VISIBLE/2;
    if(start<0)start=0;
    if(start>total-LIST_VISIBLE)start=total-LIST_VISIBLE;
    return start;
}
static void scrollbar(int selected,int total)
{
    if(total<=LIST_VISIBLE)return;
    const int rail_top=44,rail_h=69;
    int thumb=(rail_h*LIST_VISIBLE)/total;
    if(thumb<10)thumb=10;
    int offset=(rail_h-thumb)*selected/(total-1);
    display_rect(237,rail_top,2,rail_h,UI_PANEL);
    display_rect(237,rail_top+offset,2,thumb,UI_ACCENT);
}
static void clamp_selection(int *n,int count)
{
    if(count<=0){*n=0;return;}
    if(*n<0)*n=count-1;
    if(*n>=count)*n=0;
}
static bool requires_image(void)
{
    if(session.loaded)return true;
    show_message(false,"NO WORKING IMAGE","READ AN EEPROM FIRST","OR LOAD A BACKUP FROM SD","");
    return false;
}
static void status_badge(int x,const char *name,int state)
{
    display_rect(x,32,32,10,UI_BG);
    display_rect(x+3,35,4,4,state>0?UI_GOOD:state<0?UI_WARN:UI_BAD);
    display_text(x+10,34,name,UI_MUTED,1);
}
static void refresh_eeprom_detection(void)
{
    eeprom_checked=true;
    eeprom_probe_error=i2c_ok?eeprom_probe():ESP_ERR_INVALID_STATE;
    eeprom_detected=eeprom_probe_error==ESP_OK;
    ESP_LOGI(TAG,"Xbox EEPROM 0x54: %s",eeprom_detected?"DETECTED":esp_err_to_name(eeprom_probe_error));
}
static void render_menu(void)
{
    display_clear(UI_BG);
    top_rail();
    // Signature chip and two-level title retain legibility at 240x135.
    icon8(8,7,0,UI_ACCENT,2);
    display_text(31,7,"M5 PROM",UI_WHITE,2);
    display_text(31,21,"XBOX EEPROM SUITE",UI_ACCENT,1);
    display_text(142,8,"V0.9.3",UI_MUTED,1);
    display_text(139,18,"0:SET",UI_ACCENT,1);
    battery_badge();

    display_rect(6,29,228,15,UI_PANEL);
    if(session.loaded){
        display_text(12,33,"SN",UI_MUTED,1);
        fit_text(29,33,session.folder,16,UI_WHITE,1);
        display_rect(127,36,3,3,session.unsaved?UI_WARN:session.changed?UI_ACCENT:UI_GOOD);
        display_text(133,33,session.unsaved?"UNSAVED":session.is_donor?"DONOR":session.changed?"SAVED":"CLEAN",
                     session.unsaved?UI_WARN:UI_MUTED,1);
    }else{
        display_text(12,33,"NO IMAGE LOADED",UI_MUTED,1);
    }
    // BUS means the local controller initialized, not that Xbox responded.
    status_badge(167,"SD",sd_ok?1:0);
    status_badge(201,"EE",!i2c_ok?0:!eeprom_checked?-1:eeprom_detected?1:0);

    for(int i=0;i<MENU_COUNT;i++){
        int x=6+(i%MENU_COLS)*78;
        int y=48+(i/MENU_COLS)*22;
        bool selected=(i==menu_selected);
        bool write_op=(i==6);
        uint32_t ink=write_op?UI_WARN:(selected?UI_ACCENT:UI_CYAN);
        display_rect(x,y,72,20,selected?UI_FOCUS:UI_PANEL);
        display_frame(x,y,72,20,selected?UI_ACCENT:UI_LINE);
        if(selected)display_rect(x,y,3,20,UI_ACCENT);
        icon8(x+7,y+6,i,ink,1);
        display_text(x+20,y+7,menu_labels[i],selected?UI_WHITE:UI_MUTED,1);
        char idx[2]={(char)('1'+i),0};
        display_text(x+64,y+2,idx,selected?UI_ACCENT_DIM:UI_LINE,1);
    }
    display_rect(0,114,WIDTH,21,UI_HEADER);
    display_rect(0,114,WIDTH,1,UI_LINE);
    fit_text(8,116,menu_tips[menu_selected],37,
             menu_selected==6?UI_WARN:UI_WHITE,1);
    display_text(8,126,"0:SET M:FX B:BAT P:PINS",UI_MUTED,1);
    char index[4]={(char)('1'+menu_selected),'/', '9',0};
    display_text(211,126,index,UI_ACCENT,1);
}
static void render_result(void)
{
    header("OPERATION STATUS",result_good?"COMPLETE / OFFLINE WORKSPACE":"ATTENTION REQUIRED");
    display_rect(7,46,226,67,UI_PANEL);
    display_rect(7,46,3,67,result_good?UI_GOOD:UI_BAD);
    display_frame(15,52,12,12,result_good?UI_GOOD:UI_BAD);
    display_text(18,54,result_good?"+":"!",result_good?UI_GOOD:UI_BAD,1);
    fit_text(35,52,lines[0],32,result_good?UI_GOOD:UI_BAD,1);
    for(int i=1;i<4;i++)
        fit_text(16,53+i*15,lines[i],36,i==3?UI_MUTED:UI_WHITE,1);
    footer("ENTER / BACKSPACE  RETURN");
}
static const int INSPECT_COUNT=12;
static void inspect_line(int index,const image_report_t *r,char *name,size_t nc,
                         char *value,size_t vc,uint32_t *color)
{
    *color=UI_WHITE;
    const uint8_t *raw=session.working;
    switch(index){
        case 0:{char serial[13];image_extract_serial(raw,serial);snprintf(name,nc,"SERIAL");snprintf(value,vc,"%s",serial);break;}
        case 1:snprintf(name,nc,"MAC");snprintf(value,vc,"%02X%02X%02X",raw[0x40],raw[0x41],raw[0x42]);break;
        case 2:snprintf(name,nc,"FACTORY SUM");snprintf(value,vc,"%s",r->factory_ok?"VALID":"INVALID");*color=r->factory_ok?UI_GOOD:UI_BAD;break;
        case 3:snprintf(name,nc,"USER SUM");snprintf(value,vc,"%s",r->user_ok?"VALID":"INVALID");*color=r->user_ok?UI_GOOD:UI_BAD;break;
        case 4:snprintf(name,nc,"SECURITY HASH");snprintf(value,vc,"%s",r->security_ok?"VALID":"INVALID");*color=r->security_ok?UI_GOOD:UI_BAD;break;
        case 5:snprintf(name,nc,"VIDEO STD");image_edit_value(&session,IMAGE_EDIT_VIDEO_STD,value,vc);break;
        case 6:snprintf(name,nc,"GAME REGION");image_edit_value(&session,IMAGE_EDIT_GAME_REGION,value,vc);break;
        case 7:snprintf(name,nc,"LANGUAGE");image_edit_value(&session,IMAGE_EDIT_LANGUAGE,value,vc);break;
        case 8:snprintf(name,nc,"DVD REGION");image_edit_value(&session,IMAGE_EDIT_DVD_REGION,value,vc);break;
        case 9:snprintf(name,nc,"VIDEO FLAGS");snprintf(value,vc,"%s",r->video_flags_ok?"VALID":"INVALID");*color=r->video_flags_ok?UI_GOOD:UI_BAD;break;
        case 10:snprintf(name,nc,"PARENTAL");snprintf(value,vc,"%s",r->parental_ok?"VALID":"INVALID");*color=r->parental_ok?UI_GOOD:UI_BAD;break;
        case 11:snprintf(name,nc,"RATINGS");snprintf(value,vc,"%s",r->ratings_ok?"VALID":"INVALID");*color=r->ratings_ok?UI_GOOD:UI_BAD;break;
    }
}
static void render_inspect(void)
{
    header("INSPECT IMAGE",session.changed?"OFFLINE / MODIFIED":session.is_donor?"OFFLINE / DONOR":"OFFLINE / ORIGINAL");
    image_report_t report;image_report(&session,&report);
    int start=list_start(inspect_selected,INSPECT_COUNT);
    for(int i=start;i<INSPECT_COUNT&&i<start+LIST_VISIBLE;i++){
        char label[26]={0},value[29]={0};uint32_t color;
        inspect_line(i,&report,label,sizeof(label),value,sizeof(value),&color);
        list_item(LIST_FIRST_Y+(i-start)*LIST_STEP,label,i==inspect_selected,value,color);
    }
    scrollbar(inspect_selected,INSPECT_COUNT);
    footer_indexed("W/S SCROLL  BACKSPACE BACK",inspect_selected,INSPECT_COUNT);
}
static void render_edit(void)
{
    header("EDIT WORKING",session.unsaved?"UNSAVED / LOCAL CHANGES":"OFFLINE - NO CHIP WRITES");
    static const char *tab_names[IMAGE_GROUP_COUNT]={"ID","VIDEO","AUDIO","REGION","PARENT","NET"};
    for(int i=0;i<IMAGE_GROUP_COUNT;i++){
        int x=7+i*38;
        bool focus=(int)edit_group==i;
        display_rect(x,43,36,13,focus?UI_FOCUS:UI_PANEL);
        display_frame(x,43,36,13,focus?UI_ACCENT:UI_LINE);
        fit_text(x+3,47,tab_names[i],5,focus?UI_WHITE:UI_MUTED,1);
    }
    const int count=image_group_count(edit_group);
    const int first=image_group_first(edit_group);
    const int visible=5;
    int start=edit_selected-visible/2;
    if(start<0)start=0;
    if(start>count-visible)start=count>visible?count-visible:0;
    for(int i=start;i<count&&i<start+visible;i++){
        image_edit_field_t f=(image_edit_field_t)(first+i);
        char value[27]={0};image_edit_value(&session,f,value,sizeof(value));
        bool selected=i==edit_selected;
        list_item(62+(i-start)*11,image_edit_label(f),selected,value,selected?UI_CYAN:UI_GOOD);
    }
    if(count>visible){
        int thumb=(56*visible)/count;if(thumb<9)thumb=9;
        display_rect(237,59,2,56,UI_PANEL);
        display_rect(237,59+(56-thumb)*edit_selected/(count-1),2,thumb,UI_ACCENT);
    }
    footer(edit_notice?"CHANGED IN RAM | TAB NEXT CATEGORY":"TAB / 1-6 GROUP   W/S   A/D   ENTER");
}
static void render_repair(void)
{
    image_report_t r;image_report(&session,&r);
    int problems=0;
    for(int i=0;i<IMAGE_FIX_COUNT;i++)
        if(image_fix_problem(&r,(image_fix_field_t)i))problems++;
    char subtitle[38];
    if(!r.security_ok)
        snprintf(subtitle,sizeof(subtitle),"HMAC INVALID / %c FIELD FIXES",(char)('0'+problems));
    else
        snprintf(subtitle,sizeof(subtitle),"%c / 6 FIELD FIXES / RAM ONLY",(char)('0'+problems));
    header("REPAIR WORKING",subtitle);
    int start=list_start(repair_selected,IMAGE_FIX_COUNT);
    for(int i=start;i<IMAGE_FIX_COUNT&&i<start+LIST_VISIBLE;i++){
        bool problem=image_fix_problem(&r,(image_fix_field_t)i);
        list_item(LIST_FIRST_Y+(i-start)*LIST_STEP,image_fix_label((image_fix_field_t)i),
                  i==repair_selected,problem?"FIX":"OK",problem?UI_WARN:UI_GOOD);
    }
    scrollbar(repair_selected,IMAGE_FIX_COUNT);
    footer_indexed("ENTER PREVIEW  BACKSPACE BACK",repair_selected,IMAGE_FIX_COUNT);
}
static void render_text_edit(void)
{
    header("FIELD EDITOR",image_edit_label(text_field));
    display_text(11,48,"TYPE NEW VALUE",UI_MUTED,1);
    char progress[16];
    snprintf(progress,sizeof(progress),"%u / %d",(unsigned)entry_len,image_edit_text_limit(text_field));
    display_text(180,48,progress,UI_ACCENT,1);
    display_rect(8,60,224,27,UI_PANEL);
    display_frame(8,60,224,27,entry_error?UI_BAD:UI_ACCENT);
    display_text(17,70,entry_text,UI_WHITE,1);
    if(entry_len<20){
        int x=17+(int)entry_len*6;
        display_rect(x,80,5,2,entry_error?UI_BAD:UI_ACCENT);
    }
    fit_text(11,94,image_edit_text_hint(text_field),35,entry_error?UI_BAD:UI_MUTED,1);
    display_text(11,107,entry_error?"INVALID INPUT - CORRECT & RETRY":"LOCAL BUFFER ONLY - NO CHIP WRITES",entry_error?UI_BAD:UI_GOOD,1);
    footer("ENTER APPLY  BACKSPACE DELETE  ESC");
}
static void notice(const char *title,const char *line2,const char *line3,uint32_t tint)
{
    display_rect(7,47,226,65,UI_PANEL);
    display_rect(7,47,3,65,tint);
    display_rect(17,55,12,12,UI_BG);
    display_frame(17,55,12,12,tint);
    display_text(20,57,"!",tint,1);
    fit_text(35,57,title,32,UI_WHITE,1);
    fit_text(17,81,line2,35,tint,1);
    fit_text(17,96,line3,35,UI_MUTED,1);
}
static void render_confirm_fix(void)
{
    header("CONFIRM OFFLINE FIX","SAFE UNTIL YOU WRITE TO HARDWARE");
    notice(image_fix_label(pending_fix),"CHANGES WORKING COPY ONLY",
           "CHECK BEFORE SAVING",UI_WARN);
    footer("ENTER APPLY  BACKSPACE CANCEL");
}
static void render_confirm_discard(void)
{
    header("UNSAVED CHANGES","A WORKING IMAGE IS MODIFIED");
    notice("DISCARD UNSAVED WORK?","REPLACING LOSES RAM CHANGES",
           "SAVE TO SD FIRST IF NEEDED",UI_BAD);
    footer("ENTER DISCARD  BACK CANCEL");
}
static void render_browser(bool folders)
{
    int count=(int)(folders?folder_count:file_count);
    header(folders?"EEPROM FOLDERS":"SELECT EEPROM BIN",folders?"/EEPROMs/":selected_folder);
    if(count==0)display_text(12,63,folders?"NO SERIAL FOLDERS":"NO .BIN FILES",UI_WARN,1);
    int start=list_start(list_selected,count);
    for(int i=start;i<count&&i<start+LIST_VISIBLE;i++)
        list_item(LIST_FIRST_Y+(i-start)*LIST_STEP,folders?folder_list[i]:file_list[i],
                  i==list_selected,NULL,UI_WHITE);
    scrollbar(list_selected,count);
    footer_indexed("W/S MOVE  ENTER SELECT  BACK",list_selected,count);
}
static void render_battery(void)
{
    header("BATTERY", "1S LIPO / VOLTAGE ESTIMATE");
    battery_status_t b=battery_get_status();
    if(!b.valid){
        notice("BATTERY ADC UNAVAILABLE","GPIO10 / CALIBRATION FAILED", "NO CHARGE STATUS SENSOR",UI_WARN);
        footer("BACKSPACE / B TO RETURN");
        return;
    }
    const uint32_t col=battery_color(b);
    char percent[5];battery_percent_text(b,percent);
    display_text(12,49,percent,col,2);
    display_frame(88,50,140,16,UI_LINE);
    int fill=b.percentage*136/100;
    if(fill>0)display_rect(90,52,fill,12,col);
    char volts[]="VOLTAGE  0.000V";
    int mv=b.millivolts;
    if(mv<0)mv=0;
    if(mv>9999)mv=9999;
    volts[9]=(char)('0'+mv/1000);
    volts[11]=(char)('0'+(mv/100)%10);
    volts[12]=(char)('0'+(mv/10)%10);
    volts[13]=(char)('0'+mv%10);
    display_text(12,76,volts,UI_WHITE,1);
    display_text(12,91,"VOLTAGE TREND",UI_MUTED,1);
    display_text(129,91,battery_trend_name(b.trend),b.trend==BATTERY_TREND_UNKNOWN?UI_MUTED:UI_CYAN,1);
    display_text(12,105,"CHARGE/CURRENT: NOT MEASURED",UI_WARN,1);
    footer("B / BACK RETURN  -  SOC APPROX");
}

// ---------- Settings / Wi-Fi UI ----------------------------------------------
static const uint16_t dim_options[]={0,30,60,120,300};
static const char *dim_labels[]={"OFF","30 SECONDS","1 MINUTE","2 MINUTES","5 MINUTES"};
static const char *setting_names[SETTINGS_COUNT]={
    "BRIGHTNESS","AUTO DIM","SOUND EFFECTS","WIFI MODE",
    "JOIN NETWORK","WEB FILES","NETWORK INFO","REMOUNT SD","DIAGNOSTICS"
};
static const char *setting_value(int index,char *buf,size_t len)
{
    switch(index){
        case 0:snprintf(buf,len,"%u%%",(unsigned)settings_brightness());break;
        case 1:{
            int found=0;
            for(int i=0;i<5;i++)if(dim_options[i]==settings_dim_seconds())found=i;
            snprintf(buf,len,"%s",dim_labels[found]);break;
        }
        case 2:snprintf(buf,len,"%s",sound_enabled()?"ON":"OFF");break;
        case 3:snprintf(buf,len,"%s",wifi_manager_mode_name(wifi_manager_mode()));break;
        case 4:snprintf(buf,len,"SCAN");break;
        case 5:snprintf(buf,len,"%s",settings_web_enabled()?"ON":"OFF");break;
        case 6:{m5_wifi_status_t st=wifi_manager_status();
            snprintf(buf,len,"%s",st.station_connected?"ONLINE":st.ap_running?"SOFTAP":"DETAILS");break;
        }
        case 7:snprintf(buf,len,"%s",sd_ok?"MOUNTED":"RETRY");break;
        case 8:snprintf(buf,len,"OPEN");break;
        default:buf[0]=0;
    }
    return buf;
}
static void render_settings(void)
{
    header("SETTINGS","DISPLAY / WIRELESS / TRANSFER");
    int first=list_start(settings_selected,SETTINGS_COUNT);
    for(int i=first;i<SETTINGS_COUNT&&i<first+LIST_VISIBLE;i++){
        char value[24]={0};setting_value(i,value,sizeof(value));
        list_item(LIST_FIRST_Y+(i-first)*LIST_STEP,setting_names[i],
                  i==settings_selected,value,i==settings_selected?UI_CYAN:UI_GOOD);
    }
    scrollbar(settings_selected,SETTINGS_COUNT);
    footer_indexed("W/S NAV   A/D CHANGE   ENTER OPEN",settings_selected,SETTINGS_COUNT);
}
static void render_wifi_scan(void)
{
    header("SELECT WIFI","SCAN RESULTS / 2.4 GHZ");
    if(!wifi_network_count)display_text(10,70,"NO NETWORKS DETECTED",UI_WARN,1);
    int first=list_start(wifi_selected,(int)wifi_network_count);
    for(int i=first;i<(int)wifi_network_count&&i<first+LIST_VISIBLE;i++){
        char detail[18];
        snprintf(detail,sizeof(detail),"%s %d",wifi_networks[i].encrypted?"LOCK":"OPEN",
                 (int)wifi_networks[i].rssi);
        list_item(LIST_FIRST_Y+(i-first)*LIST_STEP,wifi_networks[i].ssid,
                  i==wifi_selected,detail,UI_CYAN);
    }
    scrollbar(wifi_selected,(int)wifi_network_count);
    footer_indexed("ENTER JOIN   BACK RETURN",wifi_selected,(int)wifi_network_count);
}
static void render_wifi_password(void)
{
    header("CONNECT TO WIFI","NETWORK CREDENTIALS");
    fit_text(12,48,wifi_selected_ssid,34,UI_CYAN,1);
    display_rect(8,63,224,28,UI_PANEL);
    display_frame(8,63,224,28,wifi_password_error?UI_BAD:UI_ACCENT);
    char visible[37]={0};
    size_t start=wifi_password_len>32?wifi_password_len-32:0;
    for(size_t i=start;i<wifi_password_len && i<start+32;i++)
        visible[i-start]=wifi_password_visible?wifi_password[i]:'*';
    display_text(15,73,visible,UI_WHITE,1);
    char detail[38];
    snprintf(detail,sizeof(detail),"%u CHARACTERS / TAB %s",(unsigned)wifi_password_len,
             wifi_password_visible?"HIDE":"SHOW");
    display_text(12,98,detail,UI_MUTED,1);
    display_text(12,110,wifi_password_error?"WPA2 PASSWORD: 8-63 CHARACTERS":"OPEN NETWORK: LEAVE BLANK",wifi_password_error?UI_BAD:UI_MUTED,1);
    footer("ENTER JOIN  BACK DELETE  ESC CANCEL");
}
static void render_wifi_status(void)
{
    m5_wifi_status_t st=wifi_manager_status();
    header("NETWORK INFO",settings_web_enabled()?"WEB FILE MANAGER ENABLED":"WEB FILE MANAGER DISABLED");
    char mode[40];snprintf(mode,sizeof(mode),"MODE %s",wifi_manager_mode_name(st.mode));
    display_text(13,48,mode,UI_WHITE,1);
    if(st.station_connected){
        fit_text(13,61,st.station_ssid,34,UI_CYAN,1);
        fit_text(13,74,st.station_ip,30,UI_GOOD,1);
    }else if(st.ap_running){
        fit_text(13,61,st.ap_ssid,34,UI_CYAN,1);
        fit_text(13,74,st.ap_password,28,UI_WARN,1);
        display_text(13,87,"WPA2 KEY (PRIVATE)",UI_MUTED,1);
        fit_text(13,99,st.ap_ip,28,UI_GOOD,1);
    }else{
        display_text(13,70,st.has_profile?"WAITING FOR NETWORK / FALLBACK":"NO SAVED NETWORK",UI_WARN,1);
    }
    if(web_files_running()){
        char url[40];
        snprintf(url,sizeof(url),"HTTP://%s/",st.station_connected?st.station_ip:st.ap_ip);
        fit_text(117,105,url,20,UI_ACCENT,1);
    }
    footer("BACK TO SETTINGS   HTTP / PRIVATE LAN");
}
static void open_wifi_scan(void)
{
    header("SCANNING WIFI","SEARCHING FOR 2.4 GHZ NETWORKS");
    display_text(16,68,"PLEASE WAIT...",UI_CYAN,1);
    display_present();
    // ESP-IDF supports blocking Wi-Fi scanning; do not run in an event callback.
    wifi_network_count=wifi_manager_scan(wifi_networks,sizeof(wifi_networks)/sizeof(wifi_networks[0]));
    wifi_selected=0;
    if(wifi_network_count)page=PAGE_WIFI_SCAN;
    else show_message(false,"NO WIFI NETWORKS","CHECK NETWORK RANGE","RETRY FROM SETTINGS","");
}
static void adjust_setting(int direction)
{
    switch(settings_selected){
        case 0:{
            int v=(int)settings_brightness()+direction*10;
            if(v<10)v=10;
            if(v>100)v=100;
            settings_set_brightness((uint8_t)v);
            (void)display_set_brightness((uint8_t)v);
            break;
        }
        case 1:{
            int index=0;
            for(int i=0;i<5;i++)if(dim_options[i]==settings_dim_seconds())index=i;
            index=(index+(direction<0?4:1))%5;
            settings_set_dim_seconds(dim_options[index]);break;
        }
        case 2:
            sound_set_enabled(!sound_enabled());
            if(sound_enabled())sound_notify(true);
            break;
        case 3:{
            int mode=((int)wifi_manager_mode()+(direction<0?3:1))%4;
            if(wifi_manager_set_mode((m5_wifi_mode_t)mode)!=ESP_OK)
                show_message(false,"WIFI MODE FAILED","CHECK ESP-IDF LOG","SETTINGS RETAINED","");
            break;
        }
        case 5:
            settings_set_web_enabled(!settings_web_enabled());
            web_files_tick(settings_web_enabled(),wifi_manager_network_up());
            break;
        default:break;
    }
}
static void activate_setting(void)
{
    if(settings_selected==4){open_wifi_scan();return;}
    if(settings_selected==6){page=PAGE_WIFI_STATUS;return;}
    if(settings_selected==7){
        if(web_files_running()){
            show_message(false,"WEB FILES IS ACTIVE","DISABLE WEB FILES FIRST",
                         "THEN REMOUNT THE MICROSD","");
            return;
        }
        if(!acquire_operation_or_warn())return;
        progress_reset();
        progress_draw("REMOUNT SD","FAT VFS / READBACK TEST",20,0,0,"PRESERVING EXISTING FILES");
        sd_ok=storage_remount()==ESP_OK;
        operation_guard_release();
        show_message(sd_ok,sd_ok?"MICROSD READY":"MICROSD TEST FAILED",
                     sd_ok?"FAT FS / 256B TEST PASSED":storage_last_error(),
                     sd_ok?"EXISTING FILES PRESERVED":storage_last_hint(),
                     sd_ok?"NO FORMAT PERFORMED":"CHECK SERIAL LOG / RETRY");
        return;
    }
    if(settings_selected==8){page=PAGE_DIAGNOSTICS;return;}
    adjust_setting(1);
}
static void handle_wifi_password(cp_key_event_t e)
{
    if(e.type==CP_KEY_CANCEL){page=PAGE_WIFI_SCAN;return;}
    if(e.type==CP_KEY_BACKSPACE){if(wifi_password_len)wifi_password[--wifi_password_len]=0;else page=PAGE_WIFI_SCAN;return;}
    if(e.type==CP_KEY_ENTER){
        esp_err_t err=wifi_manager_join(wifi_selected_ssid,wifi_password);
        if(err!=ESP_OK){wifi_password_error=true;sound_notify(false);return;}
        memset(wifi_password,0,sizeof(wifi_password));wifi_password_len=0;
        show_message(true,"WIFI SETTINGS SAVED","CONNECTING TO NETWORK",
                     "AUTO MAY FALL BACK TO AP","CHECK NETWORK INFO");return;
    }
    if(e.type!=CP_KEY_CHAR)return;
    if(e.character=='\t'){wifi_password_visible=!wifi_password_visible;return;}
    if((unsigned char)e.character<32||wifi_password_len>=63)return;
    wifi_password[wifi_password_len++]=e.character;
    wifi_password[wifi_password_len]=0;
    wifi_password_error=false;
}

static void render_diagnostics(void)
{
    header("DIAGNOSTICS","FAT FILESYSTEM / EEPROM 0X54");
    display_text(12,46,"SD MOUNT",UI_MUTED,1);
    display_text(92,46,storage_mounted()?"YES":"NO",storage_mounted()?UI_GOOD:UI_BAD,1);
    display_text(141,46,storage_ready()?"RW VERIFIED":"NOT VERIFIED",storage_ready()?UI_GOOD:UI_WARN,1);
    display_text(12,59,"LAST STAGE",UI_MUTED,1);
    fit_text(91,59,storage_last_stage(),24,UI_CYAN,1);
    display_text(12,72,"SD RESULT",UI_MUTED,1);
    fit_text(83,72,storage_last_error(),25,storage_ready()?UI_GOOD:UI_WARN,1);
    display_text(12,85,"I2C BUS",UI_MUTED,1);
    display_text(83,85,i2c_ok?"INITIALIZED":"NOT READY",i2c_ok?UI_GOOD:UI_BAD,1);
    display_text(12,98,"CHIP 0X54",UI_MUTED,1);
    fit_text(83,98,!eeprom_checked?"NOT YET CHECKED":eeprom_detected?"DETECTED":
             esp_err_to_name(eeprom_probe_error),25,eeprom_detected?UI_GOOD:UI_WARN,1);
    fit_text(12,110,storage_last_hint()[0]?storage_last_hint():"FAT ROOT /EEPROMs",37,UI_WARN,1);
    footer("ENTER RETEST   BACK SETTINGS");
}
static void run_diagnostics(void)
{
    if(!acquire_operation_or_warn())return;
    progress_reset();
    progress_draw("DIAGNOSTICS","FAT FILESYSTEM TEST",10,0,0,"NO CARD FORMATTING");
    sd_ok=storage_self_test()==ESP_OK;
    progress_draw("DIAGNOSTICS","EEPROM ADDRESS PROBE",75,0,0,"READ-ONLY I2C PROBE");
    refresh_eeprom_detection();
    operation_guard_release();
    page=PAGE_DIAGNOSTICS;
    if(!sd_ok||!eeprom_detected)sound_notify(false);
}
static void render_pins(void)
{
    header("GROVE / LPC","M5 PROM / ORIGINAL XBOX");
    display_text(12,45,"CARDPUTER",UI_ACCENT,1);
    display_text(171,45,"XBOX",UI_ACCENT,1);
    static const char *left[]={"G1 / SCL", "G2 / SDA", "GND", "5V / REGULATOR"};
    static const char *right[]={"PIN 13", "PIN 14", "PIN 2", "PIN 15"};
    for(int i=0;i<4;i++){
        int y=58+i*14;
        display_rect(8,y-2,224,13,i&1?UI_BG:UI_PANEL);
        display_text(14,y,left[i],i==3?UI_WARN:UI_WHITE,1);
        display_text(176,y,right[i],i==3?UI_WARN:UI_CYAN,1);
    }
    footer("PIN 15 NEEDS REGULATED 3.3V ONLY");
}
static void render_donor_mode(void)
{
    header("CREATE DONOR","CHOOSE A SOURCE - NEVER WRITES CHIP");
    static const char *modes[]={"FROM SCRATCH","COPY LOADED IMAGE","COPY IMAGE FROM SD"};
    static const char *details[]={"AUTO-SAVES NEW IMAGE TO SD","EDIT IN RAM, SAVE WHEN READY","LOAD FROM SD, EDIT, THEN SAVE"};
    for(int i=0;i<3;i++){
        list_item(49+i*18,modes[i],i==donor_mode_selected,i==0?"SD":"RAM",UI_CYAN);
    }
    fit_text(12,105,details[donor_mode_selected],36,UI_WARN,1);
    footer("W/S SELECT  ENTER CONTINUE  BACK");
}
static void render_donor(void)
{
    header("SCRATCH DONOR", "NO EEPROM IMAGE REQUIRED");
    for(int i=0;i<3;i++){
        char label[28];snprintf(label,sizeof(label),"NEW %s",donor_revision_label(i+1));
        list_item(48+i*18,label,i==donor_selected,"RETAIL",UI_CYAN);
    }
    display_text(12,106,"NEW KEY WILL NOT UNLOCK OLD HDD",UI_WARN,1);
    footer("ENTER GENERATE + AUTO-SAVE TO SD");
}
static void render_confirm_write(void)
{
    header("WRITE TO EEPROM", write_armed?"ARMED / READY FOR WRITE":"TARGET READ + BACKUP SAVED");
    display_rect(7,46,226,67,UI_PANEL);
    display_rect(7,46,3,67,write_armed?UI_WARN:UI_ACCENT);
    display_text(15,49,"TARGET",UI_MUTED,1);
    fit_text(59,49,write_target_folder,20,UI_WHITE,1);
    display_text(15,63,"SOURCE",UI_MUTED,1);
    fit_text(59,63,session.folder,20,UI_CYAN,1);
    char size_line[38];
    snprintf(size_line,sizeof(size_line),"%u BYTES / PREWRITE SAVED",(unsigned)write_diffs);
    display_text(15,79,size_line,UI_MUTED,1);
    display_text(15,94,write_revision_mismatch?"WARNING: REVISION MISMATCH":
                 write_security_changes?"WARNING: HDD / SECURITY CHANGE":"CHECK TARGET AND IMAGE",UI_WARN,1);
    footer(write_armed?"ENTER WRITE NOW / BACK CANCEL":"PRESS W TO ARM / BACK CANCEL");
}
static void render(void)
{
    switch(page){
        case PAGE_MENU:render_menu();break;
        case PAGE_RESULT:render_result();break;
        case PAGE_INSPECT:render_inspect();break;
        case PAGE_EDIT:render_edit();break;
        case PAGE_REPAIR:render_repair();break;
        case PAGE_TEXT_EDIT:render_text_edit();break;
        case PAGE_CONFIRM_FIX:render_confirm_fix();break;
        case PAGE_CONFIRM_DISCARD:render_confirm_discard();break;
        case PAGE_FOLDERS:render_browser(true);break;
        case PAGE_FILES:render_browser(false);break;
        case PAGE_PINS:render_pins();break;
        case PAGE_DONOR_MODE:render_donor_mode();break;
        case PAGE_DONOR:render_donor();break;
        case PAGE_CONFIRM_WRITE:render_confirm_write();break;
        case PAGE_BATTERY:render_battery();break;
        case PAGE_SETTINGS:render_settings();break;
        case PAGE_WIFI_SCAN:render_wifi_scan();break;
        case PAGE_WIFI_PASSWORD:render_wifi_password();break;
        case PAGE_WIFI_STATUS:render_wifi_status();break;
        case PAGE_DIAGNOSTICS:render_diagnostics();break;
    }
    display_present();
}
static void do_read(void)
{
    if(!acquire_operation_or_warn())return;
    if(!sd_ok)sd_ok=storage_init()==ESP_OK;
    if(!sd_ok){show_message(false,"SD REQUIRED FOR BACKUP",storage_last_error(),
                            storage_last_hint(),"NO CHIP READ PERFORMED");goto done;}
    if(!i2c_ok){show_message(false,"I2C NOT READY","CHECK GROVE / ADAPTER",
                             esp_err_to_name(ESP_ERR_INVALID_STATE),"");goto done;}
    progress_reset();
    progress_job_t job=OP_PROGRESS_READ;
    uint8_t raw[256]={0};
    esp_err_t err=eeprom_read_verified_progress(raw,on_eeprom_progress,&job);
    if(err!=ESP_OK){
        refresh_eeprom_detection();
        show_message(false,"EEPROM READ FAILED","TWO IDENTICAL READS NEEDED",
                     esp_err_to_name(err),"NO BACKUP WAS CREATED");goto done;
    }
    eeprom_checked=true;eeprom_detected=true;eeprom_probe_error=ESP_OK;
    progress_draw("READ EEPROM","CREATING FAT FILE",83,0,0,"/EEPROMs/<SERIAL>/");
    char folder[STORAGE_NAME_SIZE],filename[STORAGE_NAME_SIZE];
    err=storage_folder_for_raw(raw,folder,sizeof(folder));
    if(err==ESP_OK)err=storage_save_version(folder,"ORIGINAL",raw,filename,sizeof(filename));
    if(err!=ESP_OK){
        sd_ok=storage_ready();
        show_message(false,"BACKUP FAILED",storage_last_error(),
                     storage_last_hint(),"NO NEW EDIT SESSION");goto done;
    }
    progress_draw("READ EEPROM","SD FILE READBACK MATCHED",100,0,0,"256 BYTES SAVED / VERIFIED");
    image_begin(&session,raw,true);
    snprintf(session.folder,sizeof(session.folder),"%s",folder);
    char line[64];snprintf(line,sizeof(line),"/EEPROMs/%s/",folder);
    show_message(true,"ORIGINAL BACKED UP",line,filename,"LOCAL EDIT / REPAIR READY");
done:
    operation_guard_release();
}
static void do_save(void)
{
    if(!requires_image()||!acquire_operation_or_warn())return;
    if(!sd_ok)sd_ok=storage_init()==ESP_OK;
    if(!sd_ok){show_message(false,"SD NOT WRITABLE",storage_last_error(),
                            storage_last_hint(),"WORKING IMAGE STILL IN RAM");goto done;}
    progress_reset();
    progress_draw("SAVE TO SD","PREPARING FAT FILE",10,0,0,"PRESERVING ORIGINALS");
    const char *tag=session.is_donor?(session.changed?"DONOR_EDIT":"DONOR"):
                      (session.changed?"EDIT":"SNAPSHOT");
    char folder[sizeof(session.folder)];
    if(session.is_donor){
        esp_err_t err=storage_folder_for_raw(session.working,folder,sizeof(folder));
        if(err!=ESP_OK){show_message(false,"SD FOLDER FAILED",storage_last_error(),
                                    storage_last_hint(),"WORKING COPY STILL IN RAM");goto done;}
    }else snprintf(folder,sizeof(folder),"%s",session.folder);
    progress_draw("SAVE TO SD","WRITING / READING BACK",65,0,0,"256 BYTE EEPROM IMAGE");
    char filename[STORAGE_NAME_SIZE];
    esp_err_t err=storage_save_version(folder,tag,session.working,filename,sizeof(filename));
    if(err!=ESP_OK){
        sd_ok=storage_ready();
        show_message(false,"SAVE FAILED",storage_last_error(),
                     storage_last_hint(),"WORKING DATA STILL IN RAM");goto done;
    }
    progress_draw("SAVE TO SD","SD FILE VERIFIED",100,0,0,"DATA WRITTEN AND COMPARED");
    snprintf(session.folder,sizeof(session.folder),"%s",folder);
    session.unsaved=false;
    show_message(true,"SD IMAGE VERIFIED",folder,filename,"ORIGINAL FILES UNCHANGED");
done:
    operation_guard_release();
}
static void do_copy_donor(const uint8_t raw[256]);
static void open_folder_browser(void)
{
    if(!acquire_operation_or_warn())return;
    folder_count=storage_scan_folders(folder_list,STORAGE_MAX_ENTRIES);
    operation_guard_release();
    list_selected=0;page=PAGE_FOLDERS;
}
static void choose_folder(void)
{
    if(list_selected<0||list_selected>=(int)folder_count)return;
    if(!acquire_operation_or_warn())return;
    snprintf(selected_folder,sizeof(selected_folder),"%s",folder_list[list_selected]);
    file_count=storage_scan_files(selected_folder,file_list,STORAGE_MAX_ENTRIES);
    operation_guard_release();
    list_selected=0;page=PAGE_FILES;
}
static void choose_file(void)
{
    if(list_selected<0||list_selected>=(int)file_count)return;
    if(!acquire_operation_or_warn())return;
    uint8_t raw[256];const char *name=file_list[list_selected];
    progress_reset();
    progress_draw("LOAD FROM SD","READING FAT FILE",10,0,0,"VERIFYING 256 BYTE LENGTH");
    esp_err_t err=storage_load_version(selected_folder,name,raw);
    operation_guard_release();
    if(err!=ESP_OK){show_message(false,"LOAD FAILED",storage_last_error(),
                                 storage_last_hint(),esp_err_to_name(err));return;}
    progress_draw("LOAD FROM SD","256 BYTE IMAGE LOADED",100,0,0,"CHIP WAS NOT MODIFIED");
    if(browser_purpose==BROWSER_DONOR_SOURCE){
        browser_purpose=BROWSER_OPEN;
        do_copy_donor(raw);
        return;
    }
    image_begin(&session,raw,false);
    session.is_donor=strncmp(name,"DONOR_",6)==0;
    snprintf(session.folder,sizeof(session.folder),"%s",selected_folder);
    show_message(true,"IMAGE LOADED FROM SD",selected_folder,name,"OFFLINE - NO CHIP ACCESSED");
}
static void begin_text_edit(image_edit_field_t field)
{
    text_field=field;
    image_edit_value(&session,field,entry_text,sizeof(entry_text));
    if(!strcmp(entry_text,"UNKNOWN")||!strcmp(entry_text,"DISABLED"))entry_text[0]=0;
    entry_len=strlen(entry_text);
    entry_error=false;
    page=PAGE_TEXT_EDIT;
}
static void handle_entry(cp_key_event_t key)
{
    if(key.type==CP_KEY_CANCEL){page=PAGE_EDIT;entry_error=false;return;}
    if(key.type==CP_KEY_BACKSPACE){
        if(entry_len){entry_text[--entry_len]=0;entry_error=false;}
        else page=PAGE_EDIT;
        return;
    }
    if(key.type==CP_KEY_ENTER){
        if(!image_edit_text(&session,text_field,entry_text)){
            entry_error=true;sound_notify(false);return;
        }
        sound_notify(true);edit_notice=true;entry_error=false;
        page=PAGE_EDIT;return;
    }
    if(key.type!=CP_KEY_CHAR)return;
    int limit=image_edit_text_limit(text_field);
    if(limit<=0||entry_len>=(size_t)limit)return;
    char c=key.character;
    if(text_field==IMAGE_EDIT_SERIAL||text_field==IMAGE_EDIT_IP||text_field==IMAGE_EDIT_DNS||
       text_field==IMAGE_EDIT_GATEWAY||text_field==IMAGE_EDIT_SUBNET){
        if(!(c>='0'&&c<='9')&&!(c=='.'&&limit==15))return;
    }else{
        c=(char)toupper((unsigned char)c);
        if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')))return;
    }
    entry_text[entry_len++]=c;
    entry_text[entry_len]=0;
    entry_error=false;
}
static bool up(cp_key_event_t e){return e.type==CP_KEY_UP||(e.type==CP_KEY_CHAR&&(e.character=='w'||e.character=='W'));}
static bool down(cp_key_event_t e){return e.type==CP_KEY_DOWN||(e.type==CP_KEY_CHAR&&(e.character=='s'||e.character=='S'));}
static bool left(cp_key_event_t e){return e.type==CP_KEY_LEFT||(e.type==CP_KEY_CHAR&&(e.character=='a'||e.character=='A'));}
static bool right(cp_key_event_t e){return e.type==CP_KEY_RIGHT||(e.type==CP_KEY_CHAR&&(e.character=='d'||e.character=='D'));}
static bool back(cp_key_event_t e){return e.type==CP_KEY_BACKSPACE||e.type==CP_KEY_CANCEL;}

static bool working_writable(void)
{
    if(!session.loaded)return false;
    image_report_t r;image_report(&session,&r);
    return r.factory_ok&&r.user_ok&&r.security_ok&&r.security_iv>=1&&
           r.serial_ok&&r.mac_ok&&r.video_ok&&r.video_flags_ok&&
           r.language_ok&&r.dvd_ok&&r.audio_ok&&r.ratings_ok&&r.parental_ok;
}
// Donor creation and donor cloning have different persistence rules:
//   Scratch:  generate -> verify -> automatically save to microSD.
//   Clone:    copy into RAM -> edit as desired -> explicit SAVE to microSD.
// Never overwrite the original source file or mutate a source buffer.
static void adopt_donor(const uint8_t raw[256],bool copied)
{
    image_session_t validation;
    image_begin(&validation,raw,false);
    image_report_t report;
    image_report(&validation,&report);
    bool valid=report.security_ok && report.security_iv>=1 &&
               report.factory_ok && report.user_ok && report.serial_ok && report.mac_ok;
    memset(&validation,0,sizeof(validation));

    image_begin(&session,raw,false);
    session.is_donor=true;
    session.unsaved=true;
    if(copied){
        show_message(true,valid?"CLONE READY IN RAM":"CLONED DRAFT IN RAM",
                     valid?"HDD KEY / SERIAL PRESERVED":"SOURCE MAY NEED REPAIR",
                     "EDIT FIRST, THEN SELECT SAVE","SOURCE IMAGE UNCHANGED");
        return;
    }

    // Fresh donor: persist *immediately*, without needing a prior image or
    // a separate SAVE action. Keep the generated RAM image on any SD failure.
    if(!acquire_operation_or_warn())return; // RAM donor remains intact.
    if(!sd_ok)sd_ok=storage_init()==ESP_OK;
    if(!sd_ok){
        show_message(false,"SCRATCH DONOR IN RAM",storage_last_error(),
                     storage_last_hint(),"RETRY SAVE / DON'T POWER OFF");
        operation_guard_release();return;
    }
    progress_reset();
    progress_draw("SAVE DONOR","CREATING FAT32 FILE",20,0,0,"SOURCE FILES PRESERVED");
    char folder[STORAGE_NAME_SIZE],filename[STORAGE_NAME_SIZE];
    esp_err_t err=storage_folder_for_raw(raw,folder,sizeof(folder));
    if(err==ESP_OK)
        err=storage_save_version(folder,"DONOR",raw,filename,sizeof(filename));
    if(err!=ESP_OK){
        sd_ok=storage_ready();
        operation_guard_release();
        ESP_LOGW(TAG,"Scratch donor generated but SD save failed: %s",esp_err_to_name(err));
        show_message(false,"SCRATCH DONOR IN RAM",storage_last_error(),
                     storage_last_hint(),"IMAGE UNSAVED - RETRY SAVE");
        return;
    }
    progress_draw("SAVE DONOR","DONOR FILE READBACK MATCH",100,0,0,"256 BYTES VERIFIED");
    operation_guard_release();
    snprintf(session.folder,sizeof(session.folder),"%s",folder);
    session.unsaved=false;
    show_message(true,"SCRATCH DONOR SAVED",folder,filename,
                 "256 BYTES VERIFIED ON SD");
}
static void do_copy_donor(const uint8_t raw[256])
{
    // Separate buffer allows copying current working image without aliasing
    // image_begin(), which clears the old session before copying.
    uint8_t copy[256];
    if(!donor_copy_image(copy,raw)){
        show_message(false,"COPY DONOR FAILED","INVALID SOURCE BUFFER","NO IMAGE CHANGED","");
        return;
    }
    adopt_donor(copy,true);
    memset(copy,0,sizeof(copy));
}
static void do_generate_donor(void)
{
    uint8_t entropy[DONOR_RANDOM_SIZE],raw[256];
    if(battery_ok){
        esp_err_t pause_err=battery_suspend();
        if(pause_err!=ESP_OK){
            battery_ok=false;
            show_message(false,"ENTROPY NOT READY","BATTERY ADC RELEASE FAILED",
                         esp_err_to_name(pause_err),"NO IMAGE WAS REPLACED");
            return;
        }
    }
    bootloader_random_enable();
    esp_fill_random(entropy,sizeof(entropy));
    bootloader_random_disable();
    if(battery_ok)battery_ok=(battery_init()==ESP_OK);
    bool ok=donor_generate(raw,donor_selected+1,entropy);
    memset(entropy,0,sizeof(entropy));
    if(!ok){show_message(false,"DONOR CRYPTO FAILED","ENCRYPTION / CHECKSUM TEST",
                         "NO IMAGE WAS REPLACED","");return;}
    adopt_donor(raw,false);
    memset(raw,0,sizeof(raw));
}
static void do_prepare_write(void)
{
    prewrite_valid=false;write_armed=false;
    if(!requires_image()||!acquire_operation_or_warn())return;
    if(!sd_ok)sd_ok=storage_init()==ESP_OK;
    if(!sd_ok){show_message(false,"WRITE CANCELED - NO SD",storage_last_error(),
                            storage_last_hint(),"PREWRITE BACKUP REQUIRED");goto release;}
    if(!i2c_ok){show_message(false,"WRITE NOT READY","I2C REQUIRED",
                             "CHECK TARGET POWER / GROVE","");goto release;}
    if(!working_writable()){
        show_message(false,"IMAGE VALIDATION FAILED","REPAIR / INSPECT IMAGE",
                     "REQUIRES VALID RETAIL HMAC","");goto release;
    }
    progress_reset();
    progress_draw("PREPARE WRITE","VERIFYING PHYSICAL TARGET",2,0,0,"NO EEPROM WRITE YET");
    progress_job_t job=OP_PROGRESS_PREWRITE;
    esp_err_t err=eeprom_read_verified_progress(prewrite_target,on_eeprom_progress,&job);
    if(err!=ESP_OK){
        refresh_eeprom_detection();
        show_message(false,"TARGET READ FAILED","TWO MATCHING READS REQUIRED",
                     esp_err_to_name(err),"NO EEPROM WRITE PERFORMED");goto release;
    }
    eeprom_checked=true;eeprom_detected=true;eeprom_probe_error=ESP_OK;
    progress_draw("PREPARE WRITE","SAVING PREWRITE IMAGE",50,0,0,"ORIGINAL CHIP CONTENTS");
    err=storage_folder_for_raw(prewrite_target,write_target_folder,sizeof(write_target_folder));
    if(err==ESP_OK)
        err=storage_save_version(write_target_folder,"PREWRITE",prewrite_target,
                                 write_backup_name,sizeof(write_backup_name));
    if(err!=ESP_OK){
        sd_ok=storage_ready();
        show_message(false,"PREWRITE BACKUP FAILED",storage_last_error(),
                     storage_last_hint(),"HARDWARE WRITE CANCELED");goto release;
    }
    // Persist the intended image as well, so the operator can recover after a
    // partially completed write. The operation gate prevents web deletion or
    // remount of either file while waiting for the final W + ENTER consent.
    progress_draw("PREPARE WRITE","SAVING PROPOSED IMAGE",76,0,0,"TO_WRITE BACKUP");
    char source_name[STORAGE_NAME_SIZE];
    err=storage_save_version(session.folder,"TO_WRITE",session.working,
                             source_name,sizeof(source_name));
    if(err!=ESP_OK){
        sd_ok=storage_ready();
        show_message(false,"IMAGE SAVE FAILED",storage_last_error(),
                     storage_last_hint(),"HARDWARE WRITE CANCELED");goto release;
    }
    progress_draw("PREPARE WRITE","BOTH SD IMAGES VERIFIED",100,0,0,"CONFIRM BEFORE PROGRAMMING");
    session.unsaved=false;
    write_diffs=0;
    for(int i=0;i<256;i++)if(session.working[i]!=prewrite_target[i])write_diffs++;
    write_security_changes=memcmp(session.working,prewrite_target,0x30)!=0;
    image_session_t target_session;
    image_begin(&target_session,prewrite_target,true);
    image_report_t target_report;
    image_report(&target_session,&target_report);
    image_report_t desired_report;
    image_report(&session,&desired_report);
    write_revision_mismatch=target_report.security_ok&&
        (target_report.security_iv != desired_report.security_iv);
    memset(&target_session,0,sizeof(target_session));
    if(!write_diffs){
        show_message(true,"CHIP ALREADY MATCHES","NO WRITE NECESSARY",
                     "PREWRITE BACKUP SAVED",write_backup_name);goto release;
    }
    prewrite_valid=true;
    write_guard_held=true;
    page=PAGE_CONFIRM_WRITE;
    return; // Remain exclusive until confirm/cancel/verify completes.
release:
    memset(prewrite_target,0,sizeof(prewrite_target));
    operation_guard_release();
}
static void do_commit_write(void)
{
    if(!prewrite_valid||!write_armed||!write_guard_held){
        if(write_guard_held){write_guard_held=false;operation_guard_release();}
        prewrite_valid=false;write_armed=false;page=PAGE_MENU;
        return;
    }
    prewrite_valid=false;write_armed=false;
    progress_reset();
    progress_draw("WRITE EEPROM","RECHECKING PREWRITE STATE",1,0,0,"DO NOT REMOVE POWER");
    progress_job_t job=OP_PROGRESS_COMMIT;
    eeprom_write_status_t status;
    esp_err_t err=eeprom_write_verified_progress(session.working,prewrite_target,
                                                  &status,on_eeprom_progress,&job);
    memset(prewrite_target,0,sizeof(prewrite_target));
    write_guard_held=false;
    operation_guard_release();
    if(err==ESP_ERR_INVALID_STATE){
        show_message(false,"TARGET CONTENT CHANGED","WRITE CANCELED / NO CHANGES",
                     "NEW READ / BACKUP REQUIRED","");return;
    }
    if(err!=ESP_OK||!status.verified){
        char counts[38];
        snprintf(counts,sizeof(counts),"%u / %u WRITES ACKNOWLEDGED",
                 (unsigned)status.acknowledged,(unsigned)status.different);
        show_message(false,"CHIP WRITE / VERIFY FAILED",counts,
                     "CHIP MAY BE PARTLY WRITTEN","RECOVER WITH PREWRITE BACKUP");
        ESP_LOGW(TAG,"EEPROM write failed: %s, %u/%u ACKed",
                 esp_err_to_name(err),(unsigned)status.acknowledged,(unsigned)status.different);
        return;
    }
    eeprom_checked=true;eeprom_detected=true;eeprom_probe_error=ESP_OK;
    show_message(true,"EEPROM VERIFIED","FULL READBACK MATCHES",
                 write_target_folder,"POWER DOWN / DISCONNECT SAFELY");
}
static bool mount_sd_for_browser(void)
{
    if(sd_ok)return true;
    if(!acquire_operation_or_warn())return false;
    sd_ok=storage_init()==ESP_OK;
    operation_guard_release();
    return sd_ok;
}
static void select_menu(void)
{
    switch(menu_selected){
        case 0:
            if(session.unsaved){pending=PENDING_READ;page=PAGE_CONFIRM_DISCARD;}
            else do_read();
            break;
        case 1:do_save();break;
        case 2:
            if(!mount_sd_for_browser()){
                if(page==PAGE_RESULT)break; // Guard busy message already shown.
                show_message(false,"SD NOT AVAILABLE",storage_last_error(),
                                   storage_last_hint(),"");
            }else if(session.unsaved){pending=PENDING_LOAD;page=PAGE_CONFIRM_DISCARD;}
            else {browser_purpose=BROWSER_OPEN;open_folder_browser();}
            break;
        case 3:if(requires_image()){inspect_selected=0;page=PAGE_INSPECT;}break;
        case 4:if(requires_image()){edit_group=IMAGE_GROUP_IDENTITY;edit_selected=0;edit_notice=false;page=PAGE_EDIT;}break;
        case 5:if(requires_image()){repair_selected=0;page=PAGE_REPAIR;}break;
        case 6:do_prepare_write();break;
        case 7:
            if(requires_image()) {image_revert(&session);show_message(true,"WORKING COPY RESET","RESTORED LOADED IMAGE","SD FILES NOT MODIFIED","");}
            break;
        case 8:
            if(session.unsaved){pending=PENDING_DONOR;page=PAGE_CONFIRM_DISCARD;}
            else {donor_mode_selected=0;page=PAGE_DONOR_MODE;}
            break;
    }
}
static void handle_key(cp_key_event_t e)
{
    switch(page){
        case PAGE_MENU:
            if(up(e))menu_selected=(menu_selected+MENU_COUNT-MENU_COLS)%MENU_COUNT;
            else if(down(e))menu_selected=(menu_selected+MENU_COLS)%MENU_COUNT;
            else if(left(e))menu_selected=(menu_selected/MENU_COLS)*MENU_COLS+
                (menu_selected%MENU_COLS+MENU_COLS-1)%MENU_COLS;
            else if(right(e))menu_selected=(menu_selected/MENU_COLS)*MENU_COLS+
                (menu_selected%MENU_COLS+1)%MENU_COLS;
            else if(e.type==CP_KEY_CHAR&&e.character>='1'&&e.character<='9')
                menu_selected=e.character-'1';
            else if(e.type==CP_KEY_CHAR&&e.character=='0'){
                settings_selected=0;page=PAGE_SETTINGS;
            }
            else if(e.type==CP_KEY_CHAR&&(e.character=='p'||e.character=='P'))page=PAGE_PINS;
            else if(e.type==CP_KEY_CHAR&&(e.character=='b'||e.character=='B'))page=PAGE_BATTERY;
            else if(e.type==CP_KEY_CHAR&&(e.character=='m'||e.character=='M')) {
                if (sound_available()) {
                    sound_set_enabled(!sound_enabled());
                    if (sound_enabled()) sound_notify(true); /* audible unmute test */
                } else {
                    show_message(false,"SPEAKER UNAVAILABLE","I2S INITIALIZATION FAILED",
                                 "CHECK SERIAL LOG / HARDWARE","");
                }
            }
            else if(e.type==CP_KEY_ENTER)select_menu();
            break;
        case PAGE_RESULT:
            if(e.type==CP_KEY_ENTER||back(e))page=PAGE_MENU;
            break;
        case PAGE_INSPECT:
            if(up(e)){inspect_selected--;clamp_selection(&inspect_selected,INSPECT_COUNT);}
            else if(down(e)){inspect_selected++;clamp_selection(&inspect_selected,INSPECT_COUNT);}
            else if(back(e))page=PAGE_MENU;
            break;
        case PAGE_EDIT:{
            const int count=image_group_count(edit_group);
            if(e.type==CP_KEY_CHAR&&e.character=='\t'){
                edit_group=(image_edit_group_t)(((int)edit_group+1)%IMAGE_GROUP_COUNT);
                edit_selected=0;edit_notice=false;
            }else if(e.type==CP_KEY_CHAR&&e.character>='1'&&
                     e.character<(char)('1'+IMAGE_GROUP_COUNT)){
                edit_group=(image_edit_group_t)(e.character-'1');
                edit_selected=0;edit_notice=false;
            }else if(up(e)){
                edit_selected--;clamp_selection(&edit_selected,count);edit_notice=false;
            }else if(down(e)){
                edit_selected++;clamp_selection(&edit_selected,count);edit_notice=false;
            }else if(back(e))page=PAGE_MENU;
            else if(left(e)||right(e)||e.type==CP_KEY_ENTER){
                image_edit_field_t f=(image_edit_field_t)(image_group_first(edit_group)+edit_selected);
                if(image_edit_is_text(f))begin_text_edit(f);
                else {
                    bool ok=image_edit_cycle(&session,f,left(e)?-1:1);
                    if(!ok&&f==IMAGE_EDIT_GAME_REGION)
                        show_message(false,"SECURITY HASH INVALID","CAN'T RE-ENCRYPT REGION",
                                     "LOAD VALID IMAGE / BACKUP","");
                    else edit_notice=ok;
                }
            }
            break;
        }
        case PAGE_REPAIR:
            if(up(e)){repair_selected--;clamp_selection(&repair_selected,IMAGE_FIX_COUNT);}
            else if(down(e)){repair_selected++;clamp_selection(&repair_selected,IMAGE_FIX_COUNT);}
            else if(back(e))page=PAGE_MENU;
            else if(e.type==CP_KEY_ENTER){pending_fix=(image_fix_field_t)repair_selected;page=PAGE_CONFIRM_FIX;}
            break;
        case PAGE_CONFIRM_FIX:
            if(back(e))page=PAGE_REPAIR;
            else if(e.type==CP_KEY_ENTER){
                bool modified=image_apply_fix(&session,pending_fix);
                if(modified)sound_notify(true);
                page=PAGE_REPAIR;
            }
            break;
        case PAGE_CONFIRM_DISCARD:
            if(back(e)){pending=PENDING_NONE;page=PAGE_MENU;}
            else if(e.type==CP_KEY_ENTER){
                pending_t action=pending;pending=PENDING_NONE;
                if(action==PENDING_READ)do_read();
                else if(action==PENDING_LOAD){browser_purpose=BROWSER_OPEN;open_folder_browser();}
                else if(action==PENDING_DONOR){donor_mode_selected=0;page=PAGE_DONOR_MODE;}
            }
            break;
        case PAGE_TEXT_EDIT:handle_entry(e);break;
        case PAGE_FOLDERS:
            if(up(e)){list_selected--;clamp_selection(&list_selected,(int)folder_count);}
            else if(down(e)){list_selected++;clamp_selection(&list_selected,(int)folder_count);}
            else if(back(e))page=browser_purpose==BROWSER_DONOR_SOURCE?PAGE_DONOR_MODE:PAGE_MENU;
            else if(e.type==CP_KEY_ENTER)choose_folder();
            break;
        case PAGE_FILES:
            if(up(e)){list_selected--;clamp_selection(&list_selected,(int)file_count);}
            else if(down(e)){list_selected++;clamp_selection(&list_selected,(int)file_count);}
            else if(back(e)){open_folder_browser();}
            else if(e.type==CP_KEY_ENTER)choose_file();
            break;
        case PAGE_PINS:
            if(back(e)||e.type==CP_KEY_ENTER)page=PAGE_MENU;
            break;
        case PAGE_BATTERY:
            if(back(e)||e.type==CP_KEY_ENTER||
               (e.type==CP_KEY_CHAR&&(e.character=='b'||e.character=='B')))page=PAGE_MENU;
            break;
        case PAGE_DONOR_MODE:
            if(up(e))donor_mode_selected=(donor_mode_selected+2)%3;
            else if(down(e))donor_mode_selected=(donor_mode_selected+1)%3;
            else if(back(e))page=PAGE_MENU;
            else if(e.type==CP_KEY_ENTER){
                if(donor_mode_selected==0){donor_selected=0;page=PAGE_DONOR;}
                else if(donor_mode_selected==1){
                    if(!session.loaded)show_message(false,"NO IMAGE TO COPY",
                                                    "LOAD ONE OR PICK FROM SD","SCRATCH NEEDS NO IMAGE","");
                    else do_copy_donor(session.working);
                }else{
                    if(!mount_sd_for_browser()){
                        if(page==PAGE_RESULT)break;
                        show_message(false,"SD NOT AVAILABLE",storage_last_error(),
                                           storage_last_hint(),"OR USE SCRATCH MODE");
                    }else {browser_purpose=BROWSER_DONOR_SOURCE;open_folder_browser();}
                }
            }
            break;
        case PAGE_DONOR:
            if(up(e))donor_selected=(donor_selected+2)%3;
            else if(down(e))donor_selected=(donor_selected+1)%3;
            else if(back(e))page=PAGE_DONOR_MODE;
            else if(e.type==CP_KEY_ENTER)do_generate_donor();
            break;
        case PAGE_SETTINGS:
            if(up(e)){settings_selected--;clamp_selection(&settings_selected,SETTINGS_COUNT);}
            else if(down(e)){settings_selected++;clamp_selection(&settings_selected,SETTINGS_COUNT);}
            else if(back(e))page=PAGE_MENU;
            else if(left(e))adjust_setting(-1);
            else if(right(e))adjust_setting(1);
            else if(e.type==CP_KEY_ENTER)activate_setting();
            break;
        case PAGE_WIFI_SCAN:
            if(up(e)){wifi_selected--;clamp_selection(&wifi_selected,(int)wifi_network_count);}
            else if(down(e)){wifi_selected++;clamp_selection(&wifi_selected,(int)wifi_network_count);}
            else if(back(e))page=PAGE_SETTINGS;
            else if(e.type==CP_KEY_ENTER&&wifi_selected<(int)wifi_network_count){
                snprintf(wifi_selected_ssid,sizeof(wifi_selected_ssid),"%s",wifi_networks[wifi_selected].ssid);
                memset(wifi_password,0,sizeof(wifi_password));
                wifi_password_len=0;wifi_password_visible=false;wifi_password_error=false;
                page=PAGE_WIFI_PASSWORD;
            }
            break;
        case PAGE_WIFI_PASSWORD:handle_wifi_password(e);break;
        case PAGE_WIFI_STATUS:
            if(back(e)||e.type==CP_KEY_ENTER)page=PAGE_SETTINGS;
            break;
        case PAGE_DIAGNOSTICS:
            if(back(e))page=PAGE_SETTINGS;
            else if(e.type==CP_KEY_ENTER)run_diagnostics();
            break;
        case PAGE_CONFIRM_WRITE:
            if(back(e)){
                prewrite_valid=false;write_armed=false;
                memset(prewrite_target,0,sizeof(prewrite_target));
                if(write_guard_held){write_guard_held=false;operation_guard_release();}
                page=PAGE_MENU;
            }
            else if(e.type==CP_KEY_CHAR&&(e.character=='w'||e.character=='W'))write_armed=true;
            else if(e.type==CP_KEY_ENTER&&write_armed)do_commit_write();
            break;
    }
}
void app_main(void)
{
    ESP_LOGI(TAG,"M5 Prom v0.9.3 - Xbox EEPROM Suite");
    ESP_ERROR_CHECK(display_init());
    if(!operation_guard_init())ESP_LOGW(TAG,"Operation gate unavailable: SD/EEPROM actions disabled");
    settings_init();
    (void)display_set_brightness(settings_brightness());
    const uint32_t splash_start_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    display_show_splash();  /* Embedded in firmware; works with NO microSD card. */
    keyboard_init();
    (void)sound_init(); /* nonfatal and independent of SD/I2C */
    sd_ok=storage_init()==ESP_OK;
    i2c_ok=eeprom_init()==ESP_OK;
    refresh_eeprom_detection();
    // Wi-Fi is initialized before battery ADC due to one-time SoftAP key entropy.
    wifi_ok=wifi_manager_init()==ESP_OK;
    if(!wifi_ok)ESP_LOGW(TAG,"Wi-Fi unavailable; offline EEPROM functions remain usable");
    battery_ok=battery_init()==ESP_OK;
    uint32_t last_battery_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    if(battery_ok)(void)battery_tick(last_battery_ms);
    const uint32_t elapsed=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS)-splash_start_ms;
    if(elapsed<1400u)vTaskDelay(pdMS_TO_TICKS(1400u-elapsed));
    bool redraw=true,dimmed=false;
    uint32_t last_input_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    uint32_t last_net_ms=0;
    for(;;){
        const uint32_t now_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
        cp_key_event_t ev=keyboard_poll();
        if(ev.type!=CP_KEY_NONE){
            last_input_ms=now_ms;
            if(dimmed){
                dimmed=false;
                (void)display_set_brightness(settings_brightness());
            }else handle_key(ev);
            redraw=true;
        }
        uint16_t dim_delay=settings_dim_seconds();
        if(!dimmed && dim_delay>0 && (uint32_t)(now_ms-last_input_ms)>=(uint32_t)dim_delay*1000u){
            dimmed=true;
            uint8_t level=settings_brightness();
            (void)display_set_brightness(level<15?level:15);
        }
        if((uint32_t)(now_ms-last_net_ms)>=1000u){
            last_net_ms=now_ms;
            if(wifi_ok)wifi_manager_tick(now_ms);
            web_files_tick(settings_web_enabled(),wifi_manager_network_up());
            if(page==PAGE_SETTINGS||page==PAGE_WIFI_STATUS)redraw=true;
        }
        if(battery_ok&&(uint32_t)(now_ms-last_battery_ms)>=3000u){
            last_battery_ms=now_ms;
            if(battery_tick(now_ms))redraw=true;
        }
        if(redraw){render();redraw=false;}
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

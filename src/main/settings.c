#include "settings.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static uint8_t s_brightness=100;
static uint16_t s_dim_seconds=0;
static bool s_web_enabled=false;
static bool s_nvs_ready=false;
static const char *TAG="m5_settings";
static void save_u8(const char *key,uint8_t value)
{
    if(!s_nvs_ready)return;
    nvs_handle_t h;
    if(nvs_open("m5prom_ui",NVS_READWRITE,&h)!=ESP_OK)return;
    if(nvs_set_u8(h,key,value)==ESP_OK)(void)nvs_commit(h);
    nvs_close(h);
}
static void save_u16(const char *key,uint16_t value)
{
    if(!s_nvs_ready)return;
    nvs_handle_t h;
    if(nvs_open("m5prom_ui",NVS_READWRITE,&h)!=ESP_OK)return;
    if(nvs_set_u16(h,key,value)==ESP_OK)(void)nvs_commit(h);
    nvs_close(h);
}
void settings_init(void)
{
    esp_err_t e=nvs_flash_init();
    if(e!=ESP_OK){ESP_LOGW(TAG,"NVS unavailable: %s",esp_err_to_name(e));return;}
    s_nvs_ready=true;
    nvs_handle_t h;
    if(nvs_open("m5prom_ui",NVS_READONLY,&h)!=ESP_OK)return;
    (void)nvs_get_u8(h,"bright",&s_brightness);
    (void)nvs_get_u16(h,"dimsec",&s_dim_seconds);
    uint8_t w=0;
    if(nvs_get_u8(h,"web",&w)==ESP_OK)s_web_enabled=(w!=0);
    nvs_close(h);
    if(s_brightness<5||s_brightness>100)s_brightness=100;
    if(s_dim_seconds!=0&&s_dim_seconds!=30&&s_dim_seconds!=60&&
       s_dim_seconds!=120&&s_dim_seconds!=300)s_dim_seconds=0;
}
uint8_t settings_brightness(void){return s_brightness;}
void settings_set_brightness(uint8_t percent)
{
    if(percent<10)percent=10;
    if(percent>100)percent=100;
    s_brightness=percent;
    save_u8("bright",s_brightness);
}
uint16_t settings_dim_seconds(void){return s_dim_seconds;}
void settings_set_dim_seconds(uint16_t seconds)
{
    if(seconds!=0&&seconds!=30&&seconds!=60&&seconds!=120&&seconds!=300)seconds=0;
    s_dim_seconds=seconds;
    save_u16("dimsec",s_dim_seconds);
}
bool settings_web_enabled(void){return s_web_enabled;}
void settings_set_web_enabled(bool enabled){s_web_enabled=enabled;save_u8("web",enabled?1:0);}

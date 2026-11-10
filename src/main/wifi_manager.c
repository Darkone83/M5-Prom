/*
 * M5 Prom native ESP-IDF Wi-Fi: OFF / AUTO / STA / SoftAP.
 * AUTO tries saved STA credentials; after 15 s without an IP, WPA2 SoftAP
 * comes up while STA periodically retries. Upon obtaining an IP, AUTO
 * disables the SoftAP and continues in STA mode.
 */
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

static const char *TAG="m5_wifi";
/* WPA2-PSK passphrases require 8-63 characters. Workshop default. */
#define M5_PROM_AP_DEFAULT_PASSWORD "myprom83"
/* v0.9.1 stored a randomly-generated AP key with no version field.
 * Migrate that key once, without touching saved station credentials. */
#define M5_PROM_AP_KEY_VERSION 1u
static bool s_ready=false,s_started=false,s_sta_connected=false,s_ap_running=false;
static bool s_radio_state_known=true;
static bool s_driver_initialized=false,s_wifi_handler=false,s_ip_handler=false;
static bool s_auto_fallback=false;
static bool s_mode_pending=false;
static m5_wifi_mode_t s_mode=M5_WIFI_OFF;
static char s_ssid[33],s_pass[65],s_ap_ssid[33],s_ap_pass[17],s_sta_ip[16];
static esp_netif_t *s_sta_netif,*s_ap_netif;
static uint32_t s_begin_ms,s_last_retry_ms;
static uint32_t s_last_apply_ms;
static int s_attempts;
static bool s_nvs_ready;

static void save_profile(void)
{
    if(!s_nvs_ready)return;
    nvs_handle_t h;
    if(nvs_open("m5prom_wifi",NVS_READWRITE,&h)!=ESP_OK)return;
    if(nvs_set_u8(h,"mode",(uint8_t)s_mode)==ESP_OK &&
       nvs_set_str(h,"ssid",s_ssid)==ESP_OK &&
       nvs_set_str(h,"pass",s_pass)==ESP_OK &&
       nvs_set_str(h,"apkey",s_ap_pass)==ESP_OK &&
       nvs_set_u8(h,"aprev",M5_PROM_AP_KEY_VERSION)==ESP_OK)
        (void)nvs_commit(h);
    nvs_close(h);
}
static void load_profile(void)
{
    uint8_t ap_key_version=0;
    uint8_t mac[6]={0};
    (void)esp_read_mac(mac,ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid,sizeof(s_ap_ssid),"M5Prom-%02X%02X",mac[4],mac[5]);
    if(nvs_flash_init()==ESP_OK){
        s_nvs_ready=true;
        nvs_handle_t h;
        if(nvs_open("m5prom_wifi",NVS_READONLY,&h)==ESP_OK){
            uint8_t mode=M5_WIFI_OFF;
            if(nvs_get_u8(h,"mode",&mode)==ESP_OK && mode<=M5_WIFI_AP)
                s_mode=(m5_wifi_mode_t)mode;
            size_t size=sizeof(s_ssid);
            (void)nvs_get_str(h,"ssid",s_ssid,&size);
            size=sizeof(s_pass);
            (void)nvs_get_str(h,"pass",s_pass,&size);
            size=sizeof(s_ap_pass);
            (void)nvs_get_str(h,"apkey",s_ap_pass,&size);
            (void)nvs_get_u8(h,"aprev",&ap_key_version);
            nvs_close(h);
        }
    }
    if(ap_key_version!=M5_PROM_AP_KEY_VERSION || strlen(s_ap_pass)<8){
        /* This also replaces the old 12-character random password on
         * already flashed devices. No erase of NVS, SD, or STA profile. */
        snprintf(s_ap_pass,sizeof(s_ap_pass),"%s",M5_PROM_AP_DEFAULT_PASSWORD);
        save_profile();
    }
}
static void wifi_events(void *ctx,esp_event_base_t base,int32_t id,void *data)
{
    (void)ctx;
    if(base==WIFI_EVENT){
        if(id==WIFI_EVENT_STA_START){
            if(s_started && s_ssid[0] && s_mode!=M5_WIFI_OFF && s_mode!=M5_WIFI_AP)
                (void)esp_wifi_connect();
        }else if(id==WIFI_EVENT_STA_DISCONNECTED){
            const bool was_connected=s_sta_connected;
            s_sta_connected=false;
            s_sta_ip[0]=0;
            if(s_started && s_ssid[0] && s_mode!=M5_WIFI_OFF && s_mode!=M5_WIFI_AP){
                const uint32_t now=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
                if(was_connected)s_begin_ms=now;
                s_last_retry_ms=now;
                if(!s_mode_pending && ++s_attempts<=3)(void)esp_wifi_connect();
            }
        }else if(id==WIFI_EVENT_AP_START){
            if(s_started && s_mode!=M5_WIFI_OFF && s_mode!=M5_WIFI_STATION)
                s_ap_running=true;
        }
        else if(id==WIFI_EVENT_AP_STOP)s_ap_running=false;
    }else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        /* Ignore queued events from a station that has since been stopped. */
        if(!s_started || s_mode==M5_WIFI_OFF || s_mode==M5_WIFI_AP)return;
        ip_event_got_ip_t *ev=(ip_event_got_ip_t*)data;
        if(ev){
            snprintf(s_sta_ip,sizeof(s_sta_ip),IPSTR,IP2STR(&ev->ip_info.ip));
            s_sta_connected=true;
            s_attempts=0;
        }
    }
}
static esp_err_t set_ap_configuration(void)
{
    wifi_config_t cfg={0};
    memcpy(cfg.ap.ssid,s_ap_ssid,strlen(s_ap_ssid));
    cfg.ap.ssid_len=(uint8_t)strlen(s_ap_ssid);
    memcpy(cfg.ap.password,s_ap_pass,strlen(s_ap_pass));
    cfg.ap.channel=1;
    cfg.ap.max_connection=4;
    cfg.ap.authmode=WIFI_AUTH_WPA2_PSK;
    cfg.ap.pmf_cfg.required=false;
    return esp_wifi_set_config(WIFI_IF_AP,&cfg);
}
static esp_err_t set_sta_configuration(void)
{
    wifi_config_t cfg={0};
    memcpy(cfg.sta.ssid,s_ssid,strlen(s_ssid));
    memcpy(cfg.sta.password,s_pass,strlen(s_pass));
    cfg.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.threshold.authmode=WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA,&cfg);
}
/* Keep RF ownership separate from the selected mode and link status. In
 * particular a temporary scan can run the radio with the UI set to OFF.
 * A failed stop must never grant the donor generator permission to use SAR
 * entropy. If start fails partway, stop it before reporting RF as stopped. */
static esp_err_t stop_radio(void)
{
    esp_err_t e=esp_wifi_stop();
    if(e==ESP_OK){
        s_started=false;
        s_radio_state_known=true;
        s_sta_connected=false;s_ap_running=false;s_sta_ip[0]=0;
    }else s_radio_state_known=false;
    return e;
}
static esp_err_t start_radio(void)
{
    s_started=true; /* Remain conservative until a successful stop. */
    s_radio_state_known=false;
    esp_err_t e=esp_wifi_start();
    if(e==ESP_OK)s_radio_state_known=true;
    else{
        esp_err_t stopped=stop_radio();
        if(stopped!=ESP_OK)
            ESP_LOGW(TAG,"Wi-Fi start cleanup failed: %s",esp_err_to_name(stopped));
    }
    return e;
}
static esp_err_t apply_mode(void)
{
    if(!s_ready)return ESP_ERR_INVALID_STATE;
    s_mode_pending=true;
    s_last_apply_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    if(s_started){
        esp_err_t err=stop_radio();
        if(err!=ESP_OK)return err;
    }
    s_sta_connected=false;s_ap_running=false;s_auto_fallback=false;
    s_sta_ip[0]=0;s_attempts=0;
    s_begin_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
    s_last_retry_ms=s_begin_ms;
    wifi_mode_t mode=WIFI_MODE_NULL;
    if(s_mode==M5_WIFI_AP || (s_mode==M5_WIFI_AUTO && !s_ssid[0]))
        mode=WIFI_MODE_AP;
    else if(s_mode==M5_WIFI_AUTO||s_mode==M5_WIFI_STATION)
        mode=WIFI_MODE_STA;
    esp_err_t err=esp_wifi_set_mode(mode);
    if(err!=ESP_OK)goto failed;
    if(mode==WIFI_MODE_NULL){s_mode_pending=false;return ESP_OK;}
    if(mode==WIFI_MODE_STA){err=set_sta_configuration();}
    else {err=set_ap_configuration();}
    if(err!=ESP_OK)goto failed;
    err=start_radio();
    if(err!=ESP_OK)goto failed;
    s_mode_pending=false;
    return err;
failed:
    if(!s_started)(void)esp_wifi_set_mode(WIFI_MODE_NULL);
    return err;
}
static void cleanup_init(void)
{
    /* These are only our handlers/interfaces; the process-wide event loop
     * and esp-netif service may belong to other application components. */
    if(s_ip_handler && esp_event_handler_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,
                                                     &wifi_events)==ESP_OK)
        s_ip_handler=false;
    if(s_wifi_handler && esp_event_handler_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,
                                                       &wifi_events)==ESP_OK)
        s_wifi_handler=false;
    if(s_driver_initialized){
        esp_err_t e=esp_wifi_deinit();
        if(e!=ESP_OK){
            ESP_LOGW(TAG,"Wi-Fi init cleanup failed: %s",esp_err_to_name(e));
            return; /* Retain ownership so a later retry cannot init twice. */
        }
        s_driver_initialized=false;
        s_started=false;s_radio_state_known=true;
    }
    if(s_sta_netif){esp_netif_destroy_default_wifi(s_sta_netif);s_sta_netif=NULL;}
    if(s_ap_netif){esp_netif_destroy_default_wifi(s_ap_netif);s_ap_netif=NULL;}
}
esp_err_t wifi_manager_init(void)
{
    if(s_ready)return (s_mode_pending||!s_radio_state_known)?apply_mode():ESP_OK;
    esp_err_t e=esp_netif_init();
    if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    e=esp_event_loop_create_default();
    if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    if(!s_sta_netif)s_sta_netif=esp_netif_create_default_wifi_sta();
    if(!s_sta_netif){e=ESP_ERR_NO_MEM;goto failed;}
    if(!s_ap_netif)s_ap_netif=esp_netif_create_default_wifi_ap();
    if(!s_ap_netif){e=ESP_ERR_NO_MEM;goto failed;}
    if(!s_driver_initialized){
        wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
        e=esp_wifi_init(&cfg);
        if(e!=ESP_OK)goto failed;
        s_driver_initialized=true;
    }
    e=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(e!=ESP_OK)goto failed;
    if(!s_wifi_handler){
        e=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_events,NULL);
        if(e!=ESP_OK)goto failed;
        s_wifi_handler=true;
    }
    if(!s_ip_handler){
        e=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_events,NULL);
        if(e!=ESP_OK)goto failed;
        s_ip_handler=true;
    }
    load_profile();
    s_ready=true;
    e=apply_mode();
    if(e!=ESP_OK){ESP_LOGW(TAG,"Wi-Fi mode apply failed: %s",esp_err_to_name(e));}
    else ESP_LOGI(TAG,"Wi-Fi initialized; mode=%s",wifi_manager_mode_name(s_mode));
    return e;
failed:
    ESP_LOGW(TAG,"Wi-Fi initialization failed: %s",esp_err_to_name(e));
    cleanup_init();
    return e;
}
esp_err_t wifi_manager_set_mode(m5_wifi_mode_t mode)
{
    if(mode<M5_WIFI_OFF||mode>M5_WIFI_AP)return ESP_ERR_INVALID_ARG;
    if(!s_ready)return ESP_ERR_INVALID_STATE;
    s_mode=mode;
    save_profile();
    return apply_mode();
}
m5_wifi_mode_t wifi_manager_mode(void){return s_mode;}
esp_err_t wifi_manager_join(const char *ssid,const char *password)
{
    if(!s_ready||!ssid||!password)return ESP_ERR_INVALID_ARG;
    size_t sn=strlen(ssid),pn=strlen(password);
    if(sn<1||sn>32||pn>63||(pn>0&&pn<8))return ESP_ERR_INVALID_ARG;
    memcpy(s_ssid,ssid,sn+1);
    memcpy(s_pass,password,pn+1);
    if(s_mode==M5_WIFI_OFF||s_mode==M5_WIFI_AP)s_mode=M5_WIFI_AUTO;
    save_profile();
    return apply_mode();
}
size_t wifi_manager_scan(m5_wifi_scan_entry_t *out,size_t max_entries)
{
    if(!s_ready||!s_radio_state_known||!out||!max_entries)return 0;
    wifi_mode_t existing=WIFI_MODE_NULL;
    if(esp_wifi_get_mode(&existing)!=ESP_OK)return 0;
    const bool was_started=s_started;
    bool changed_mode=false;
    if(existing==WIFI_MODE_NULL){
        if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK)return 0;
        changed_mode=true;
    }else if(existing==WIFI_MODE_AP){
        if(esp_wifi_set_mode(WIFI_MODE_APSTA)!=ESP_OK)return 0;
        changed_mode=true;
    }
    size_t count=0;
    if(!was_started && start_radio()!=ESP_OK)goto restore;
    if(!was_started||changed_mode)vTaskDelay(pdMS_TO_TICKS(250));
    wifi_scan_config_t scan={0};
    esp_err_t err=esp_wifi_scan_start(&scan,true);
    bool records_released=false;
    uint16_t total=0;
    if(err==ESP_OK && esp_wifi_scan_get_ap_num(&total)==ESP_OK){
        if(total>24)total=24;
        wifi_ap_record_t *records=calloc(total?total:1,sizeof(*records));
        if(records){
            if(esp_wifi_scan_get_ap_records(&total,records)==ESP_OK){
                records_released=true;
                for(int i=0;i<total&&count<max_entries;i++){
                    if(!records[i].ssid[0])continue;
                    m5_wifi_scan_entry_t *p=&out[count++];
                    memset(p,0,sizeof(*p));
                    snprintf(p->ssid,sizeof(p->ssid),"%.32s",(const char*)records[i].ssid);
                    p->rssi=records[i].rssi;
                    p->encrypted=records[i].authmode!=WIFI_AUTH_OPEN;
                }
            }
            free(records);
        }
    }
    /* ESP-IDF owns the AP list until successful retrieval or explicit clear.
     * Failed count/retrieval/allocation paths must not leak scan memory. */
    if(err==ESP_OK && !records_released){
        esp_err_t cleared=esp_wifi_clear_ap_list();
        if(cleared!=ESP_OK)ESP_LOGW(TAG,"Wi-Fi scan cleanup failed: %s",esp_err_to_name(cleared));
    }
restore:
    if(!was_started && s_started){
        esp_err_t stopped=stop_radio();
        if(stopped!=ESP_OK){
            s_mode_pending=true;
            s_last_apply_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
            ESP_LOGW(TAG,"Temporary scan radio still running: %s",esp_err_to_name(stopped));
            return count;
        }
    }
    if(changed_mode){
        esp_err_t restored=esp_wifi_set_mode(existing);
        if(restored!=ESP_OK){
            s_mode_pending=true;
            s_last_apply_ms=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);
            ESP_LOGW(TAG,"Scan mode restore failed: %s",esp_err_to_name(restored));
        }
    }
    return count;
}
void wifi_manager_tick(uint32_t now_ms)
{
    if(!s_ready)return;
    /* A failed mode change can stop RF before failing to restart it. Keep a
     * bounded retry even when stopped, including recovery from a failed stop.
     * Initialization itself is still retried only by an explicit UI action. */
    if(s_mode_pending || !s_radio_state_known){
        if((uint32_t)(now_ms-s_last_apply_ms)>=5000u)(void)apply_mode();
        return;
    }
    if(!s_started||!s_ssid[0] ||
       (s_mode!=M5_WIFI_AUTO&&s_mode!=M5_WIFI_STATION))return;
    if(s_sta_connected){
        if(s_mode==M5_WIFI_AUTO && s_auto_fallback){
            if(esp_wifi_set_mode(WIFI_MODE_STA)==ESP_OK)s_auto_fallback=false;
        }
        return;
    }
    if(s_mode==M5_WIFI_AUTO && !s_auto_fallback && (uint32_t)(now_ms-s_begin_ms)>=15000u){
        // Configure WPA2 before opening the AP; Wi-Fi may reject AP config
        // when the driver is still in station-only mode.
        if(stop_radio()==ESP_OK){
            if(esp_wifi_set_mode(WIFI_MODE_APSTA)==ESP_OK &&
               set_ap_configuration()==ESP_OK &&
               set_sta_configuration()==ESP_OK &&
               start_radio()==ESP_OK){
                s_auto_fallback=true;
                s_last_retry_ms=now_ms;
                ESP_LOGI(TAG,"STA unavailable; WPA2 SoftAP fallback active");
            }else{
                s_mode_pending=true;s_last_apply_ms=now_ms;
                ESP_LOGW(TAG,"Auto fallback failed; retrying radio setup in 5 seconds");
            }
        }else {s_mode_pending=true;s_last_apply_ms=now_ms;}
        if(s_mode_pending)return;
    }
    if((s_auto_fallback||s_mode==M5_WIFI_STATION) &&
       (uint32_t)(now_ms-s_last_retry_ms)>=60000u){
        s_last_retry_ms=now_ms;
        s_attempts=0;
        (void)esp_wifi_connect();
    }
}
m5_wifi_status_t wifi_manager_status(void)
{
    m5_wifi_status_t s={0};
    s.mode=s_mode;s.station_connected=s_sta_connected;s.ap_running=s_ap_running;
    s.has_profile=s_ssid[0]!=0;
    snprintf(s.station_ssid,sizeof(s.station_ssid),"%s",s_ssid);
    snprintf(s.station_ip,sizeof(s.station_ip),"%s",s_sta_ip);
    snprintf(s.ap_ssid,sizeof(s.ap_ssid),"%s",s_ap_ssid);
    snprintf(s.ap_password,sizeof(s.ap_password),"%s",s_ap_pass);
    if(s_ap_netif){
        esp_netif_ip_info_t ip={0};
        if(esp_netif_get_ip_info(s_ap_netif,&ip)==ESP_OK)
            snprintf(s.ap_ip,sizeof(s.ap_ip),IPSTR,IP2STR(&ip.ip));
    }
    return s;
}
bool wifi_manager_network_up(void){return s_ready&&(s_sta_connected||s_ap_running);}
bool wifi_manager_radio_running(void){return s_started;}
bool wifi_manager_radio_state_known(void){return s_radio_state_known;}
const char *wifi_manager_mode_name(m5_wifi_mode_t mode)
{
    switch(mode){case M5_WIFI_AUTO:return "AUTO";case M5_WIFI_STATION:return "STATION";
        case M5_WIFI_AP:return "SOFTAP";default:return "OFF";}
}

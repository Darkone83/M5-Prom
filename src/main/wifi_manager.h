#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    M5_WIFI_OFF=0,
    M5_WIFI_AUTO=1,
    M5_WIFI_STATION=2,
    M5_WIFI_AP=3
} m5_wifi_mode_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool encrypted;
} m5_wifi_scan_entry_t;

typedef struct {
    m5_wifi_mode_t mode;
    bool station_connected;
    bool ap_running;
    bool has_profile;
    char station_ssid[33];
    char station_ip[16];
    char ap_ssid[33];
    char ap_password[17];
    char ap_ip[16];
} m5_wifi_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_set_mode(m5_wifi_mode_t mode);
m5_wifi_mode_t wifi_manager_mode(void);
esp_err_t wifi_manager_join(const char *ssid,const char *password);
size_t wifi_manager_scan(m5_wifi_scan_entry_t *out,size_t max_entries);
void wifi_manager_tick(uint32_t now_ms);
m5_wifi_status_t wifi_manager_status(void);
bool wifi_manager_network_up(void);
const char *wifi_manager_mode_name(m5_wifi_mode_t mode);

/* Cardputer V1.1 battery gauge: GPIO10 -> ADC1_CH9 (2:1 divider).
 * Native ESP-IDF 5.5.x oneshot ADC + calibrated curve-fit readout.
 * This is a voltage-based estimate, NOT a current/charger-state sensor.
 */
#include "battery.h"
#include <stddef.h>
#include "board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG="battery";
static adc_oneshot_unit_handle_t adc1=NULL;
static adc_cali_handle_t calibration=NULL;
static bool ready=false;
static battery_model_t estimator;
static bool model_initialized=false;
static unsigned consecutive_failed_ticks=0;

/* Do not keep displaying a stale percentage indefinitely if USB-only power,
 * ADC read errors or the battery sense circuit become unavailable. A single
 * noisy burst should not blank the status, so require three failed polls.
 */
static bool battery_sample_failed(void)
{
    if(consecutive_failed_ticks<3u)consecutive_failed_ticks++;
    if(consecutive_failed_ticks<3u)return false;
    if(!estimator.status.valid)return false;
    battery_model_init(&estimator);
    ESP_LOGW(TAG,"No valid battery samples for 3 polls; display unavailable");
    return true;
}


esp_err_t battery_init(void)
{
    if(ready)return ESP_OK;
    if(!model_initialized){battery_model_init(&estimator);model_initialized=true;}
    adc_unit_t unit=ADC_UNIT_1;
    adc_channel_t chan=ADC_CHANNEL_9;
    esp_err_t err=adc_oneshot_io_to_channel(CP_BAT_ADC_GPIO,&unit,&chan);
    if(err!=ESP_OK||unit!=ADC_UNIT_1||chan!=ADC_CHANNEL_9){
        ESP_LOGW(TAG,"Battery ADC pin mapping unexpected");
        return err==ESP_OK?ESP_ERR_INVALID_STATE:err;
    }
    const adc_oneshot_unit_init_cfg_t config={.unit_id=ADC_UNIT_1};
    err=adc_oneshot_new_unit(&config,&adc1);
    if(err!=ESP_OK)return err;
    const adc_oneshot_chan_cfg_t chan_cfg={
        .atten=ADC_ATTEN_DB_12,
        .bitwidth=ADC_BITWIDTH_12
    };
    err=adc_oneshot_config_channel(adc1,ADC_CHANNEL_9,&chan_cfg);
    if(err!=ESP_OK)goto fail;

    // ESP32-S3 curve-fitting calibration. Do not invent a raw-to-mV mapping
    // if factory calibration is unavailable; show battery as unavailable.
    const adc_cali_curve_fitting_config_t cal_cfg={
        .unit_id=ADC_UNIT_1,
        .chan=ADC_CHANNEL_9,
        .atten=ADC_ATTEN_DB_12,
        .bitwidth=ADC_BITWIDTH_12
    };
    err=adc_cali_create_scheme_curve_fitting(&cal_cfg,&calibration);
    if(err!=ESP_OK){
        ESP_LOGW(TAG,"ADC calibration unavailable: %s",esp_err_to_name(err));
        goto fail;
    }
    ready=true;
    consecutive_failed_ticks=0;
    ESP_LOGI(TAG,"GPIO%d / ADC1 CH9 calibrated 2:1 battery voltage sense",CP_BAT_ADC_GPIO);
    return ESP_OK;
fail:
    if(adc1){adc_oneshot_del_unit(adc1);adc1=NULL;}
    return err;
}

esp_err_t battery_suspend(void)
{
    // Hardware entropy collection via bootloader_random_enable() cannot run
    // while the SAR ADC controller is in use. Refuse donor creation if the
    // ADC cannot be cleanly released (never overlap the peripherals).
    ready=false;
    if(calibration){
        esp_err_t err=adc_cali_delete_scheme_curve_fitting(calibration);
        if(err!=ESP_OK)return err;
        calibration=NULL;
    }
    if(adc1){
        esp_err_t err=adc_oneshot_del_unit(adc1);
        if(err!=ESP_OK)return err;
        adc1=NULL;
    }
    return ESP_OK;
}

bool battery_tick(uint32_t now_ms)
{
    if(!ready)return false;
    int voltage_samples[7];
    unsigned good=0;
    for(unsigned i=0;i<7;i++) {
        int raw=0,mv=0;
        if(adc_oneshot_read(adc1,ADC_CHANNEL_9,&raw)!=ESP_OK)continue;
        if(adc_cali_raw_to_voltage(calibration,raw,&mv)!=ESP_OK)continue;
        // ADC reads divided VBAT, so convert pin voltage to battery voltage.
        // Physical measurement is uncertain during charging or heavy load.
        const int vbatt_mv=mv*2;
        if(vbatt_mv>=2500&&vbatt_mv<=4500)
            voltage_samples[good++]=vbatt_mv;
    }
    if(good<4)return battery_sample_failed();
    consecutive_failed_ticks=0;
    // Sort and take median to reject sporadic SPI/audio/charger noise spikes.
    for(unsigned i=1;i<good;i++){
        int n=voltage_samples[i];unsigned j=i;
        while(j>0&&voltage_samples[j-1]>n){voltage_samples[j]=voltage_samples[j-1];j--;}
        voltage_samples[j]=n;
    }
    return battery_model_sample(&estimator,voltage_samples[good/2],now_ms);
}

battery_status_t battery_get_status(void)
{
    return estimator.status;
}

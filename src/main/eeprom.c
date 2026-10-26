#include "eeprom.h"
#include "board.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "xbox_eeprom";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t eeprom_init(void)
{
    // Board/Grove pull-ups must be verified; a target-referenced pull-up and power-isolation
    // scheme belongs on the external adapter. Internal pull-ups are not a substitute.
    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CP_GROVE_SCL,
        .sda_io_num = CP_GROVE_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    esp_err_t err = i2c_new_master_bus(&config, &s_bus);
    if (err != ESP_OK) return err;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XBOX_EEPROM_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device, &s_dev);
    ESP_LOGI(TAG, "I2C configured at 100kHz on GPIO%d SCL / GPIO%d SDA", CP_GROVE_SCL, CP_GROVE_SDA);
    return err;
}

esp_err_t eeprom_probe(void)
{
    if (!s_bus) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(s_bus, XBOX_EEPROM_I2C_ADDRESS, 100);
}

static esp_err_t read_once(uint8_t out[256])
{
    // Read the entire 24C02 in one sequential-read transaction.
    // This uses a memory-address byte but never transmits EEPROM content.
    uint8_t offset = 0;
    return i2c_master_transmit_receive(s_dev, &offset, 1, out, XBOX_EEPROM_SIZE, 300);
}

static void send_progress(eeprom_progress_cb_t cb,void *user,
                          eeprom_progress_phase_t phase,unsigned done,unsigned total)
{
    if(cb)cb(user,phase,done,total);
}

static esp_err_t read_verified_impl(uint8_t out[256],eeprom_progress_cb_t cb,
                                    void *user,bool post_write)
{
    if (!s_dev || !out) return ESP_ERR_INVALID_ARG;
    if(!post_write) send_progress(cb,user,EEPROM_PROGRESS_PROBE,0,2);
    esp_err_t err = eeprom_probe();
    if (err != ESP_OK) return err;

    uint8_t second[256];
    err = read_once(out);
    if (err != ESP_OK) return err;
    send_progress(cb,user,post_write?EEPROM_PROGRESS_VERIFY_FIRST:EEPROM_PROGRESS_READ_FIRST,1,2);
    err = read_once(second);
    if (err != ESP_OK) return err;
    send_progress(cb,user,post_write?EEPROM_PROGRESS_VERIFY_SECOND:EEPROM_PROGRESS_READ_SECOND,2,2);
    if (memcmp(out, second, 256) != 0) {
        ESP_LOGE(TAG, "Two reads differed; refuse backup / verification");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!post_write) send_progress(cb,user,EEPROM_PROGRESS_READ_MATCH,2,2);
    return ESP_OK;
}

esp_err_t eeprom_read_verified_progress(uint8_t out[256],
                                        eeprom_progress_cb_t cb,void *user)
{
    return read_verified_impl(out,cb,user,false);
}

esp_err_t eeprom_read_verified(uint8_t out[256])
{
    return eeprom_read_verified_progress(out,NULL,NULL);
}

/*
 * Physical 24C02 programmer. Each transaction writes one byte at a time;
 * this is intentionally slower than page writes to avoid assuming the EEPROM
 * manufacturer's page size. EEPROM ACK polling waits for the end of the
 * internal programming cycle, including the final byte.
 *
 * The caller MUST already have committed a verified PREWRITE backup to SD.
 * A second read-compare here closes the gap between confirmation and write.
 */
esp_err_t eeprom_write_verified_progress(const uint8_t desired[256],
                                         const uint8_t expected_before[256],
                                         eeprom_write_status_t *status,
                                         eeprom_progress_cb_t cb,void *user)
{
    if(status)memset(status,0,sizeof(*status));
    if(!s_dev||!s_bus||!desired||!expected_before||!status)return ESP_ERR_INVALID_ARG;

    uint8_t current[256];
    esp_err_t err=read_verified_impl(current,cb,user,false);
    if(err!=ESP_OK)return err;
    if(memcmp(current,expected_before,256)!=0){
        ESP_LOGE(TAG,"EEPROM changed since PREWRITE snapshot; refusing write");
        return ESP_ERR_INVALID_STATE;
    }
    for(int i=0;i<256;i++)if(current[i]!=desired[i])status->different++;
    if(status->different==0){
        status->verified=true;
        send_progress(cb,user,EEPROM_PROGRESS_VERIFIED,0,0);
        return ESP_OK;
    }

    ESP_LOGW(TAG,"Beginning %u EEPROM byte writes. Do not remove power.",
             (unsigned)status->different);
    send_progress(cb,user,EEPROM_PROGRESS_PROGRAM,0,status->different);
    for(int i=0;i<256;i++){
        if(current[i]==desired[i])continue;
        const uint8_t tx[2]={(uint8_t)i,desired[i]};
        err=i2c_master_transmit(s_dev,tx,sizeof(tx),150);
        if(err!=ESP_OK){
            ESP_LOGE(TAG,"Write failed at 0x%02X after %u bytes",i,(unsigned)status->acknowledged);
            return err;
        }
        // During EEPROM's internal write cycle it will not ACK. Poll with
        // a generous 200+ ms ceiling for noisy in-circuit buses.
        bool ready=false;
        for(int tries=0;tries<25;tries++){
            vTaskDelay(pdMS_TO_TICKS(10));
            if(i2c_master_probe(s_bus,XBOX_EEPROM_I2C_ADDRESS,30)==ESP_OK){
                ready=true;break;
            }
        }
        if(!ready){
            ESP_LOGE(TAG,"Write-cycle timeout at 0x%02X; chip may be partially modified",i);
            return ESP_ERR_TIMEOUT;
        }
        status->acknowledged++;
        send_progress(cb,user,EEPROM_PROGRESS_PROGRAM,status->acknowledged,status->different);
    }

    uint8_t verify[256];
    err=read_verified_impl(verify,cb,user,true);
    if(err!=ESP_OK)return err;
    if(memcmp(verify,desired,256)!=0){
        ESP_LOGE(TAG,"POST-WRITE MISMATCH; DO NOT REMOVE POWER BEFORE RECOVERY");
        return ESP_ERR_INVALID_RESPONSE;
    }
    status->verified=true;
    send_progress(cb,user,EEPROM_PROGRESS_VERIFIED,status->acknowledged,status->different);
    ESP_LOGI(TAG,"EEPROM programmed and fully verified (%u different bytes)",
             (unsigned)status->different);
    return ESP_OK;
}

esp_err_t eeprom_write_verified(const uint8_t desired[256],
                                const uint8_t expected_before[256],
                                eeprom_write_status_t *status)
{
    return eeprom_write_verified_progress(desired,expected_before,status,NULL,NULL);
}

uint32_t eeprom_crc32(const uint8_t data[256])
{
    // File fingerprint only. NOT an Xbox EEPROM/HMAC/RC4 integrity check.
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < 256; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

/* M5 Prom Remote: ESP32-S3 native USB-OTG vendor bulk interface.
 * Prototype device IDs ONLY. Allocate a proper VID/PID before distribution.
 * Native ROM download mode remains recoverable through BOOT+RESET; no eFuses.
 *
 * Endpoint IN 0x81: screen updates, status, SD download
 * Endpoint OUT 0x01: keyboard, heartbeat, SD upload
 * Protocol payload is the unchanged CRC-protected M5 Prom v1 stream.
 */
#include "remote_native_usb.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "remote_timing.h"
#include <string.h>

#define M5_USB_VID 0xCAFEu /* development VID; not licensed for shipping */
#define M5_USB_PID 0x4055u
#define M5_WINUSB_REQUEST 0x20u
#define M5_WINUSB_DESCRIPTOR_LENGTH 0xB2u
#define M5_BOS_LENGTH (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define M5_CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

#if !CFG_TUD_VENDOR
#error "Enable CONFIG_TINYUSB_VENDOR_COUNT=1 in sdkconfig"
#endif

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, .bDeviceClass = 0, .bDeviceSubClass = 0,
    .bDeviceProtocol = 0, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = M5_USB_VID, .idProduct = M5_USB_PID, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};
static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, M5_CONFIG_LENGTH, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
};
static const char *s_strings[] = {
    (const char[]) {0x09, 0x04},
    "Darkone Customs", "M5 Prom Remote (Native USB)", NULL, "M5 Prom Bulk",
};
/* Microsoft OS 2.0 platform capability: Windows binds WinUSB to interface 0. */
static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(M5_BOS_LENGTH, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(M5_WINUSB_DESCRIPTOR_LENGTH, M5_WINUSB_REQUEST),
};
uint8_t const *tud_descriptor_bos_cb(void) { return s_bos; }

/* 0xB2-byte compatible-ID + DeviceInterfaceGUIDs registry property set.
   The stable GUID identifies the M5 Prom vendor interface to Windows. */
static const uint8_t s_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(M5_WINUSB_DESCRIPTOR_LENGTH),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(M5_WINUSB_DESCRIPTOR_LENGTH-0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    0, 0, U16_TO_U8S_LE(M5_WINUSB_DESCRIPTOR_LENGTH-0x0A-0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W','I','N','U','S','B',0,0, 0,0,0,0,0,0,0,0,
    U16_TO_U8S_LE(M5_WINUSB_DESCRIPTOR_LENGTH-0x0A-0x08-0x08-0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,
    'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,'s',0,0,0,
    U16_TO_U8S_LE(0x0050),
    /* {88CB8D7A-353E-442A-81C7-1BCBC53D9665} */
    '{',0,'8',0,'8',0,'C',0,'B',0,'8',0,'D',0,'7',0,'A',0,'-',0,
    '3',0,'5',0,'3',0,'E',0,'-',0,'4',0,'4',0,'2',0,'A',0,'-',0,
    '8',0,'1',0,'C',0,'7',0,'-',0,'1',0,'B',0,'C',0,'B',0,'C',0,
    '5',0,'3',0,'D',0,'9',0,'6',0,'6',0,'5',0,'}',0,0,0,0,0,
};
_Static_assert(sizeof(s_ms_os_20)==M5_WINUSB_DESCRIPTOR_LENGTH,"Microsoft USB descriptor size");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if(stage!=CONTROL_STAGE_SETUP)return true;
    if(request->bmRequestType_bit.type==TUSB_REQ_TYPE_VENDOR &&
       request->bRequest==M5_WINUSB_REQUEST && request->wIndex==7){
        return tud_control_xfer(rhport,request,(void *)(uintptr_t)s_ms_os_20,sizeof(s_ms_os_20));
    }
    return false;
}

static const char *TAG="m5_native_usb";
static TaskHandle_t s_remote_task;
static volatile uint32_t s_tx_completed;
void remote_native_set_task(TaskHandle_t task) { s_remote_task=task; }
void remote_native_wake(void)
{
    TaskHandle_t task=s_remote_task;
    if(task)xTaskNotifyGive(task);
}
void remote_native_wait(uint32_t timeout_ms)
{
    /* Minimum one tick even when CONFIG_FREERTOS_HZ=100 and requested <10ms. */
    (void)ulTaskNotifyTake(pdTRUE,m5_remote_wait_ticks(timeout_ms));
}
uint32_t remote_native_sent_bytes(void) { return s_tx_completed; }
/* TinyUSB callbacks run in its device task, not an interrupt.
 * esp_tinyusb releases differ on the third tud_vendor_rx_cb argument:
 * some declare uint16_t and newer sources declare uint32_t. The ESP-IDF
 * GCC toolchain can select the exact declared parameter type, avoiding
 * conflicting definitions when the managed component changes versions.
 */
#define M5_VENDOR_RX_U16 __builtin_types_compatible_p( \
    __typeof__(&tud_vendor_rx_cb), void (*)(uint8_t, const uint8_t *, uint16_t))
#define M5_VENDOR_RX_U32 __builtin_types_compatible_p( \
    __typeof__(&tud_vendor_rx_cb), void (*)(uint8_t, const uint8_t *, uint32_t))
_Static_assert(M5_VENDOR_RX_U16 || M5_VENDOR_RX_U32,
               "Unsupported TinyUSB vendor RX callback signature");
typedef __typeof__(__builtin_choose_expr(M5_VENDOR_RX_U16,
                                         (uint16_t)0, (uint32_t)0)) m5_vendor_rx_size_t;
void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buf, m5_vendor_rx_size_t size)
{
    (void)buf;(void)size;
    if(itf==0)remote_native_wake();
}
esp_err_t remote_native_init(void)
{
    tinyusb_config_t cfg=TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device=&s_device;
    cfg.descriptor.full_speed_config=s_config;
    cfg.descriptor.string=s_strings;
    cfg.descriptor.string_count=sizeof(s_strings)/sizeof(s_strings[0]);
    esp_err_t err=tinyusb_driver_install(&cfg);
    if(err==ESP_OK){
        ESP_LOGI(TAG,"USB OTG vendor mode active; 0x%04x:0x%04x",M5_USB_VID,M5_USB_PID);
        ESP_LOGI(TAG,"Vendor buffers: TX=%u RX=%u endpoint=%u bytes",
                 (unsigned)CONFIG_TINYUSB_VENDOR_TX_BUFSIZE,
                 (unsigned)CONFIG_TINYUSB_VENDOR_RX_BUFSIZE,
                 (unsigned)CONFIG_TINYUSB_VENDOR_EPSIZE);
        if(CONFIG_TINYUSB_VENDOR_EPSIZE<2048)
            ESP_LOGW(TAG,"USB endpoint buffer below 2048; check active sdkconfig (not just sdkconfig.defaults)");
        if(CONFIG_TINYUSB_VENDOR_TX_BUFSIZE<8192)
            ESP_LOGW(TAG,"USB TX FIFO below 8192; batching adapts, but throughput may be limited");
    }
    return err;
}
bool remote_native_connected(void)
{
    return tud_mounted() && tud_vendor_n_mounted(0) && !tud_suspended();
}
int remote_native_read(uint8_t *dst,size_t capacity)
{
    if(!dst||!capacity||!remote_native_connected())return 0;
    const uint32_t n=tud_vendor_n_available(0);
    if(!n)return 0;
    size_t want=n<capacity?n:capacity;
    return (int)tud_vendor_n_read(0,dst,(uint32_t)want);
}
bool remote_native_write_packet(const uint8_t *src,size_t size,uint32_t timeout_ms)
{
    if(!src||!size)return false;
    const TickType_t start=xTaskGetTickCount();
    /* Check free capacity BEFORE enqueueing so protocol packets remain atomic.
     * The TinyUSB device task services the vendor FIFO asynchronously. */
    while(remote_native_connected()){
        if(tud_vendor_n_write_available(0)>=size){
            const uint32_t accepted=tud_vendor_n_write(0,src,(uint32_t)size);
            if(accepted!=size)return false;
            (void)tud_vendor_n_write_flush(0);
            return true;
        }
        if((uint32_t)((xTaskGetTickCount()-start)*portTICK_PERIOD_MS)>=timeout_ms)return false;
        vTaskDelay(1); /* Real tick: a 1 ms conversion may round to zero. */
    }
    return false;
}

/* Reserve at least 512 bytes of FIFO for keys, protocol status, and
 * diagnostics while screen data is queued. No partial v1 packet is written,
 * and a full bulk IN FIFO cannot stall the foreground user interface. */
#define M5_SCREEN_CONTROL_HEADROOM ((CONFIG_TINYUSB_VENDOR_TX_BUFSIZE>=1024)?512u:0u)
bool remote_native_write_screen_batch(const uint8_t *src,size_t size)
{
    if(!src||!size||!remote_native_connected())return false;
    if(size > CONFIG_TINYUSB_VENDOR_TX_BUFSIZE-M5_SCREEN_CONTROL_HEADROOM)return false;
    if(tud_vendor_n_write_available(0) < size+M5_SCREEN_CONTROL_HEADROOM)return false;
    if(tud_vendor_n_write(0,src,(uint32_t)size)!=(uint32_t)size)return false;
    (void)tud_vendor_n_write_flush(0); /* One flush for a group of row packets. */
    return true;
}

/* The vendor driver automatically submits the remaining FIFO data on each
 * completed transfer (vendor_device.c). Wake the producer; do NOT issue an
 * extra flush from inside the completion callback. */
void tud_vendor_tx_cb(uint8_t itf,uint32_t sent_bytes)
{
    if(itf==0){
        s_tx_completed+=sent_bytes;
        remote_native_wake();
    }
}

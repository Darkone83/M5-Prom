/* M5 Prom Remote v1 - ESP32-S3 native USB-OTG vendor bridge.
 * Device owns FAT filesystem at all times; PC never sees USB mass storage.
 * All incoming remote keys go through the existing UI event handler.
 */
#include "remote_usb.h"
#include "remote_protocol.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#ifndef CONFIG_TINYUSB_VENDOR_TX_BUFSIZE
#define CONFIG_TINYUSB_VENDOR_TX_BUFSIZE 8192 /* host-test-only default */
#endif
#include "board.h"
#include "storage.h"
#include "operation_guard.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "remote_native_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define FB_BYTES (CP_LCD_WIDTH*CP_LCD_HEIGHT*2u)
#define REM_TIMEOUT_MS 6500u
#define REM_UPLOAD_LIMIT (16u*1024u*1024u)
#define REM_TX_PAYLOAD 512u
#define REM_FRAME_ROWS_PER_TICK 24u /* bounded row work per wake; RX/keys remain serviceable */
/* The current active sdkconfig may be older than sdkconfig.defaults. The
 * screen batch must fit the ACTUAL TinyUSB TX FIFO, including control headroom. */
#if CONFIG_TINYUSB_VENDOR_TX_BUFSIZE < 512
#error "M5 Prom Remote requires TinyUSB vendor TX FIFO >= 512 bytes; use menuconfig"
#endif
#define REM_SCREEN_HEADROOM ((CONFIG_TINYUSB_VENDOR_TX_BUFSIZE>=1024)?512u:0u)
#define REM_SCREEN_BATCH_BYTES ((CONFIG_TINYUSB_VENDOR_TX_BUFSIZE-REM_SCREEN_HEADROOM)<4096u ? \
                                (CONFIG_TINYUSB_VENDOR_TX_BUFSIZE-REM_SCREEN_HEADROOM) : 4096u)
#define REM_MAX_PATH 256
#define REM_ACK_QUEUE_DEPTH 64u

static const char *TAG="m5_remote";
static SemaphoreHandle_t s_lock;
typedef struct {
    cp_key_event_t event;
    uint16_t seq;
    uint32_t received_ms;
} rem_key_t;
typedef struct {
    uint16_t seq;
    uint8_t stage;
    uint16_t device_delay_ms;
} rem_ack_t;
static QueueHandle_t s_key_queue;
static QueueHandle_t s_ack_queue;
static volatile uint32_t s_received_bytes,s_frames_completed,s_keys_queued;
static volatile uint32_t s_keys_rendered,s_key_drops,s_screen_backpressure;
static volatile uint32_t s_publish_lock_misses,s_ack_drops;
static volatile uint16_t s_key_queue_peak;
static uint32_t s_polled_key_received_ms;
static uint16_t s_polled_key_sequence=PROM_KEY_SEQ_LEGACY;
static bool s_perf_requested;
static uint16_t *s_snapshot; /* allocated only if bridge starts */
static uint32_t s_snapshot_generation;
static bool s_snapshot_ready;
static volatile bool s_active;
static uint32_t s_last_host_ms;
static uint16_t s_frame_id;
static uint32_t s_row_hash[CP_LCD_HEIGHT];
/* USB task exclusively owns this staging buffer. Allocate statically to avoid
 * putting a 4 KiB transfer buffer on its 6144-byte FreeRTOS task stack. */
static uint8_t s_screen_batch[REM_SCREEN_BATCH_BYTES];
static uint16_t s_frame_y;
static bool s_frame_in_progress;
static bool s_frame_started;
static bool s_fast_rows;
static uint32_t s_frame_gen;
static FILE *s_transfer;
static bool s_upload;
static char s_final_path[REM_MAX_PATH];
static char s_temp_path[REM_MAX_PATH];
static uint32_t s_expected,s_transferred;
static bool s_guard_held;
static int (*s_log_printf)(const char*,va_list);

static int suppress_log(const char *fmt,va_list args)
{(void)fmt;(void)args;return 0;}
static uint32_t ticks_ms(void)
{return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);}
static uint32_t hash_row(const uint8_t *data,size_t len)
{
    uint32_t h=2166136261u;
    for(size_t i=0;i<len;i++){h^=data[i];h*=16777619u;}
    return h;
}
static bool tx_packet_timeout(uint8_t type,const void *data,uint16_t len,uint32_t timeout_ms)
{
    uint8_t frame[PROM_MAX_PAYLOAD+7];
    size_t n=prom_encode(type,data,len,frame,sizeof(frame));
    if(!n)return false;
    /* Small, atomic packet enqueue: the USB stack drains its TX FIFO without
       serial baudrate emulation or blocking for hundreds of milliseconds. */
    return remote_native_write_packet(frame,n,timeout_ms);
}
static bool tx_packet(uint8_t type,const void *data,uint16_t len)
{
    return tx_packet_timeout(type,data,len,12u);
}
/* One batch contains several independently CRC-protected v1 screen packets.
 * Existing desktop decoders already accept multiple packets per USB read. */
static bool batch_append(size_t *used,uint8_t type,const void *data,uint16_t len)
{
    if(*used>sizeof(s_screen_batch))return false;
    const size_t n=prom_encode(type,data,len,s_screen_batch+*used,
                               sizeof(s_screen_batch)-*used);
    if(!n)return false;
    *used+=n;
    return true;
}
static void transfer_close(bool discard)
{
    if(s_transfer){fclose(s_transfer);s_transfer=NULL;}
    if(s_upload&&discard&&s_temp_path[0])unlink(s_temp_path);
    s_upload=false;s_expected=0;s_transferred=0;
    s_final_path[0]=s_temp_path[0]='\0';
    if(s_guard_held){operation_guard_release();s_guard_held=false;}
}
static void fs_done(uint8_t code,const char *message)
{
    uint8_t out[120];out[0]=code;
    size_t n=message?strlen(message):0;
    if(n>sizeof(out)-2)n=sizeof(out)-2;
    if(n)memcpy(out+1,message,n);
    out[n+1]=0;
    (void)tx_packet(PROM_FS_DONE,out,(uint16_t)(n+2));
}
static bool fs_acquire(void)
{
    if(!storage_ready()){fs_done(PROM_NO_SD,"SD not ready; check Diagnostics");return false;}
    if(!operation_guard_try_acquire()){fs_done(PROM_BUSY,"SD busy with backup, write or transfer");return false;}
    s_guard_held=true;return true;
}
static bool fs_path(const uint8_t *payload,size_t n,char full[REM_MAX_PATH],bool allow_root)
{
    if(n==0||n>PROM_SD_MAX_RELATIVE||memchr(payload,0,n))return false;
    char rel[PROM_SD_MAX_RELATIVE+1];
    memcpy(rel,payload,n);rel[n]='\0';
    if(!allow_root && strcmp(rel,"/")==0)return false;
    return prom_sd_path(rel,full,REM_MAX_PATH);
}
static void fs_list(const uint8_t *bytes,uint16_t n)
{
    char path[REM_MAX_PATH];
    if(!fs_path(bytes,n,path,true)){fs_done(PROM_BAD_PATH,"Invalid folder path");return;}
    if(!fs_acquire())return;
    DIR *dir=opendir(path);
    if(!dir){transfer_close(false);fs_done(PROM_NOT_FOUND,"Folder not found");return;}
    bool good=true;
    uint16_t count=0;
    struct dirent *e;
    while((e=readdir(dir))!=NULL&&count<400){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        size_t name_len=strlen(e->d_name);
        if(name_len==0||name_len>240)continue;
        char item[REM_MAX_PATH];
        if(snprintf(item,sizeof(item),"%s/%s",path,e->d_name)>=(int)sizeof(item))continue;
        struct stat st;
        if(stat(item,&st)!=0)continue;
        uint8_t reply[1+4+241];
        reply[0]=S_ISDIR(st.st_mode)?1:0;
        uint32_t size=S_ISDIR(st.st_mode)?0:(uint32_t)st.st_size;
        reply[1]=(uint8_t)size;reply[2]=(uint8_t)(size>>8);
        reply[3]=(uint8_t)(size>>16);reply[4]=(uint8_t)(size>>24);
        memcpy(reply+5,e->d_name,name_len);
        if(!tx_packet(PROM_FS_ENTRY,reply,(uint16_t)(5+name_len))){good=false;break;}
        ++count;
    }
    closedir(dir);transfer_close(false);
    if(good)fs_done(PROM_OK,count==400?"Showing first 400 entries":"Folder loaded");
}
static void fs_get(const uint8_t *bytes,uint16_t n)
{
    char path[REM_MAX_PATH];
    if(!fs_path(bytes,n,path,false)){fs_done(PROM_BAD_PATH,"Invalid file path");return;}
    if(!fs_acquire())return;
    struct stat st;
    if(stat(path,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>REM_UPLOAD_LIMIT){
        transfer_close(false);fs_done(PROM_NOT_FOUND,"File unavailable or too large");return;
    }
    s_transfer=fopen(path,"rb");
    if(!s_transfer){transfer_close(false);fs_done(PROM_IO_ERROR,"Cannot open file");return;}
    s_expected=(uint32_t)st.st_size;s_transferred=0;s_upload=false;
    uint8_t size[4]={(uint8_t)s_expected,(uint8_t)(s_expected>>8),
                     (uint8_t)(s_expected>>16),(uint8_t)(s_expected>>24)};
    if(!tx_packet(PROM_FS_GET_BEGIN,size,4))transfer_close(false);
}
static void fs_put_begin(const uint8_t *bytes,uint16_t n)
{
    if(n<6){fs_done(PROM_BAD_PATH,"Upload name missing");return;}
    uint32_t total=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|
                   ((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
    if(total>REM_UPLOAD_LIMIT){fs_done(PROM_TOO_BIG,"16 MB transfer limit");return;}
    char path[REM_MAX_PATH];
    if(!fs_path(bytes+4,n-4,path,false)){fs_done(PROM_BAD_PATH,"Invalid upload destination");return;}
    if(!fs_acquire())return;
    struct stat st;
    if(stat(path,&st)==0){transfer_close(false);fs_done(PROM_EXISTS,"Destination already exists");return;}
    if(snprintf(s_temp_path,sizeof(s_temp_path),"%s.M5TMP",path)>=(int)sizeof(s_temp_path)){
        transfer_close(false);fs_done(PROM_BAD_PATH,"Destination name too long");return;
    }
    int fd=open(s_temp_path,O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0){transfer_close(false);fs_done(PROM_IO_ERROR,"Cannot create temporary file");return;}
    s_transfer=fdopen(fd,"wb");
    if(!s_transfer){close(fd);unlink(s_temp_path);transfer_close(false);fs_done(PROM_IO_ERROR,"Cannot open temporary file");return;}
    snprintf(s_final_path,sizeof(s_final_path),"%s",path);
    s_expected=total;s_transferred=0;s_upload=true;
    uint8_t ready=0;(void)tx_packet(PROM_FS_READY,&ready,1);
}
static void fs_put_chunk(const uint8_t *bytes,uint16_t n)
{
    if(!s_upload||!s_transfer||!s_guard_held){fs_done(PROM_BAD_STATE,"No upload active");return;}
    if(!n||n>REM_TX_PAYLOAD||s_transferred+n>s_expected){
        transfer_close(true);fs_done(PROM_TOO_BIG,"Upload length invalid");return;
    }
    if(fwrite(bytes,1,n,s_transfer)!=n){
        transfer_close(true);fs_done(PROM_IO_ERROR,"SD write failed");return;
    }
    s_transferred+=n;
    const uint8_t ack[4]={(uint8_t)s_transferred,(uint8_t)(s_transferred>>8),
                          (uint8_t)(s_transferred>>16),(uint8_t)(s_transferred>>24)};
    (void)tx_packet(PROM_FS_ACK,ack,4);
}
static void fs_put_done(void)
{
    if(!s_upload||!s_transfer){fs_done(PROM_BAD_STATE,"No upload to finish");return;}
    if(s_transferred!=s_expected){transfer_close(true);fs_done(PROM_IO_ERROR,"Upload incomplete");return;}
    if(fflush(s_transfer)!=0||fsync(fileno(s_transfer))!=0){
        transfer_close(true);fs_done(PROM_IO_ERROR,"SD sync failed");return;
    }
    if(fclose(s_transfer)!=0){s_transfer=NULL;transfer_close(true);fs_done(PROM_IO_ERROR,"SD close failed");return;}
    s_transfer=NULL;
    struct stat st;
    if(stat(s_temp_path,&st)!=0||(uint32_t)st.st_size!=s_expected ||
       access(s_final_path,F_OK)==0||rename(s_temp_path,s_final_path)!=0){
        transfer_close(true);fs_done(PROM_IO_ERROR,"SD finalize failed");return;
    }
    transfer_close(false);fs_done(PROM_OK,"Upload complete");
}
static void fs_simple(uint8_t type,const uint8_t *bytes,uint16_t n)
{
    char first[REM_MAX_PATH],second[REM_MAX_PATH];
    const uint8_t *other=NULL;
    size_t first_len=n,second_len=0;
    if(type==PROM_FS_RENAME){
        other=memchr(bytes,0,n);
        if(!other){fs_done(PROM_BAD_PATH,"Missing rename target");return;}
        first_len=(size_t)(other-bytes);other++;
        second_len=n-first_len-1;
    }
    if(!fs_path(bytes,first_len,first,false)||
       (type==PROM_FS_RENAME&&!fs_path(other,second_len,second,false))){
        fs_done(PROM_BAD_PATH,"Invalid SD path");return;
    }
    if(!fs_acquire())return;
    struct stat st;
    uint8_t status=PROM_OK;
    switch(type){
        case PROM_FS_MKDIR:
            if(mkdir(first,0775)!=0)status=errno==EEXIST?PROM_EXISTS:PROM_IO_ERROR;
            break;
        case PROM_FS_DELETE:
            if(stat(first,&st)!=0)status=PROM_NOT_FOUND;
            else if((S_ISDIR(st.st_mode)?rmdir(first):unlink(first))!=0)status=PROM_IO_ERROR;
            break;
        case PROM_FS_RENAME:
            if(stat(second,&st)==0)status=PROM_EXISTS;
            else if(rename(first,second)!=0)status=PROM_IO_ERROR;
            break;
    }
    transfer_close(false);
    fs_done(status,status==PROM_OK?"SD operation complete":"SD operation failed");
}
static void session_off(void)
{
    s_active=false;
    transfer_close(true);
    if(s_log_printf){esp_log_set_vprintf(s_log_printf);s_log_printf=NULL;}
    if(s_key_queue)xQueueReset(s_key_queue);
    if(s_ack_queue)xQueueReset(s_ack_queue);
    s_perf_requested=false;
    s_frame_in_progress=false;s_frame_started=false;s_snapshot_ready=false;
    s_fast_rows=false;
}
static void ack_key(uint16_t sequence,uint8_t stage,uint32_t delay_ms)
{
    if(sequence==PROM_KEY_SEQ_LEGACY || !s_ack_queue)return;
    rem_ack_t ack={sequence,stage,(uint16_t)(delay_ms>65535u?65535u:delay_ms)};
    if(xQueueSend(s_ack_queue,&ack,0)!=pdTRUE){
        s_ack_drops++; /* Account for lost *diagnostic* ACKs separately. */
        return;
    }
    remote_native_wake();
}
/* Measurable on-device counters; no host/device clock synchronization assumed.
 * v1 payload: version, reserved, tick_ms, twelve uint32s, two uint16s. */
static void put16(uint8_t *out,uint16_t n)
{out[0]=(uint8_t)n;out[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *out,uint32_t n)
{for(unsigned i=0;i<4;i++)out[i]=(uint8_t)(n>>(i*8));}
static void send_perf(void)
{
    uint8_t data[PROM_PERF_WIRE_SIZE]={0};
    data[0]=1;
    put16(data+2,(uint16_t)portTICK_PERIOD_MS);
    const uint32_t values[12]={
        ticks_ms(),s_received_bytes,remote_native_sent_bytes(),s_frames_completed,
        s_keys_queued,s_keys_rendered,s_key_drops,s_screen_backpressure,
        s_publish_lock_misses,s_ack_drops,
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    };
    for(unsigned i=0;i<12;i++)put32(data+4+i*4,values[i]);
    put16(data+52,s_key_queue_peak);
    const UBaseType_t stack_free=uxTaskGetStackHighWaterMark(NULL);
    put16(data+54,(uint16_t)(stack_free>65535u?65535u:stack_free));
    if(tx_packet_timeout(PROM_PERF_REPLY,data,sizeof(data),0u))s_perf_requested=false;
}
static void send_pending_diagnostics(void)
{
    if(s_ack_queue){
        for(unsigned i=0;i<4;i++){
            rem_ack_t item;
            if(xQueuePeek(s_ack_queue,&item,0)!=pdTRUE)break;
            uint8_t bytes[5];
            put16(bytes,item.seq);bytes[2]=item.stage;
            put16(bytes+3,item.device_delay_ms);
            if(!tx_packet_timeout(PROM_KEY_STATUS,bytes,sizeof(bytes),0u))break;
            (void)xQueueReceive(s_ack_queue,&item,0);
        }
    }
    if(s_perf_requested)send_perf();
}
static void on_command(const prom_packet_t *p)
{
    if(p->type==PROM_HELLO){
        if(p->length!=1||p->payload[0]!=PROM_PROTOCOL_VERSION)return;
        if(!s_active){
            s_active=true;
            s_log_printf=esp_log_set_vprintf(suppress_log);
        }
        s_last_host_ms=ticks_ms();
        s_snapshot_ready=false;s_frame_in_progress=false;s_frame_started=false;
        s_fast_rows=false;
        memset(s_row_hash,0,sizeof(s_row_hash));
        uint8_t ack[6]={PROM_PROTOCOL_VERSION,
                       (uint8_t)CP_LCD_WIDTH,(uint8_t)(CP_LCD_WIDTH>>8),
                       (uint8_t)CP_LCD_HEIGHT,(uint8_t)(CP_LCD_HEIGHT>>8),0x07|PROM_CAP_RLE|PROM_CAP_PERF};
        (void)tx_packet(PROM_HELLO_ACK,ack,sizeof(ack));
        return;
    }
    if(!s_active)return;
    s_last_host_ms=ticks_ms();
    if(p->type==PROM_PING){(void)tx_packet(PROM_PONG,NULL,0);return;}
    if(p->type==PROM_PERF_REQUEST){
        if(p->length==0){s_perf_requested=true;remote_native_wake();}
        return;
    }
    if(p->type==PROM_QUIT){session_off();return;}
    if(p->type==PROM_SCREEN_FULL || p->type==PROM_SCREEN_FAST){
        if(p->type==PROM_SCREEN_FAST){
            if(p->length!=1)return;
            s_fast_rows=(p->payload[0]==1u);
        }
        memset(s_row_hash,0,sizeof(s_row_hash));
        s_frame_in_progress=false;s_frame_started=false;
        if(s_snapshot_generation)s_snapshot_ready=true;
        return;
    }
    if(p->type==PROM_KEY){
        if((p->length==2||p->length==4)&&
           p->payload[0]>=CP_KEY_CHAR&&p->payload[0]<=CP_KEY_CANCEL){
            const uint16_t seq=p->length==4 ?
                (uint16_t)((uint16_t)p->payload[2]|((uint16_t)p->payload[3]<<8)):
                PROM_KEY_SEQ_LEGACY;
            rem_key_t key={{(cp_key_type_t)p->payload[0],(char)p->payload[1]},seq,ticks_ms()};
            if(s_key_queue&&xQueueSend(s_key_queue,&key,0)==pdTRUE){
                s_keys_queued++;
                const UBaseType_t pending=uxQueueMessagesWaiting(s_key_queue);
                if(pending>s_key_queue_peak)s_key_queue_peak=(uint16_t)pending;
                ack_key(seq,PROM_KEY_QUEUED,0);
            }else{
                s_key_drops++;
                ack_key(seq,PROM_KEY_DROPPED,0);
            }
        }
        return;
    }
    /* One active file transfer at a time. Preserve SD gate across its lifetime. */
    if(p->type==PROM_FS_CANCEL){transfer_close(true);fs_done(PROM_OK,"Transfer cancelled");return;}
    if(s_transfer && p->type!=PROM_FS_PUT_CHUNK&&p->type!=PROM_FS_PUT_DONE){
        fs_done(PROM_BUSY,"Transfer in progress");return;
    }
    switch(p->type){
        case PROM_FS_LIST:fs_list(p->payload,p->length);break;
        case PROM_FS_GET:fs_get(p->payload,p->length);break;
        case PROM_FS_PUT_BEGIN:fs_put_begin(p->payload,p->length);break;
        case PROM_FS_PUT_CHUNK:fs_put_chunk(p->payload,p->length);break;
        case PROM_FS_PUT_DONE:fs_put_done();break;
        case PROM_FS_MKDIR:case PROM_FS_DELETE:case PROM_FS_RENAME:
            fs_simple(p->type,p->payload,p->length);break;
        default:break;
    }
}
static void download_tick(void)
{
    if(!s_transfer||s_upload)return;
    uint8_t chunk[REM_TX_PAYLOAD];
    uint32_t remaining=s_expected-s_transferred;
    size_t want=remaining>sizeof(chunk)?sizeof(chunk):(size_t)remaining;
    if(want){
        size_t got=fread(chunk,1,want,s_transfer);
        if(got!=want){transfer_close(false);fs_done(PROM_IO_ERROR,"SD read failed");return;}
        if(!tx_packet(PROM_FS_DATA,chunk,(uint16_t)got))return;
        s_transferred+=(uint32_t)got;
    }
    if(s_transferred==s_expected){transfer_close(false);fs_done(PROM_OK,"Download complete");}
}
static void frame_tick(void)
{
    if(!s_snapshot||!s_lock||!s_snapshot_ready)return;
    if(!s_frame_in_progress){
        s_frame_id++;
        s_frame_y=0;
        s_frame_in_progress=true;
        s_frame_started=false;
        s_frame_gen=s_snapshot_generation;
    }

    /* Stage up to 24 changed rows (or 4096 encoded bytes) and submit them
     * together. Never commit a row hash or advance s_frame_y until the ENTIRE
     * batch has entered the USB FIFO. On backpressure the same data is retried
     * next wake: no missing rows, duplicate FRAME_START, or partial v1 packet. */
    uint8_t committed_rows[REM_FRAME_ROWS_PER_TICK];
    uint32_t committed_hashes[REM_FRAME_ROWS_PER_TICK];
    unsigned changed=0;
    size_t batch_size=0;
    bool begun=s_frame_started;
    uint16_t next_y=s_frame_y;
    while(next_y<CP_LCD_HEIGHT && changed<REM_FRAME_ROWS_PER_TICK){
        const uint16_t row=next_y;
        uint8_t pixels[CP_LCD_WIDTH*2];
        if(xSemaphoreTake(s_lock,0)!=pdTRUE)break; /* don't delay foreground rendering */
        memcpy(pixels,(const uint8_t*)s_snapshot+row*sizeof(pixels),sizeof(pixels));
        xSemaphoreGive(s_lock);
        const uint32_t h=hash_row(pixels,sizeof(pixels));
        if(h!=s_row_hash[row]){
            uint8_t payload[3+CP_LCD_WIDTH*2+8];
            payload[0]=(uint8_t)s_frame_id;
            payload[1]=(uint8_t)(s_frame_id>>8);
            payload[2]=(uint8_t)row;
            uint8_t type=PROM_FRAME_ROW;
            uint16_t size=3u+sizeof(pixels);
            if(s_fast_rows){
                const size_t packed=prom_rle_encode565(pixels,CP_LCD_WIDTH,
                                            payload+3,sizeof(payload)-3);
                if(packed && packed<sizeof(pixels)){
                    type=PROM_FRAME_RLE;
                    size=(uint16_t)(3u+packed);
                }
            }
            if(type==PROM_FRAME_ROW)memcpy(payload+3,pixels,sizeof(pixels));

            /* A 2-byte FRAME_START is encoded as 9 bytes. Leave space for
             * that and the complete row before staging either packet. */
            const size_t required=(size_t)size+7u+(begun?0u:9u);
            if(required>sizeof(s_screen_batch)-batch_size)break;
            if(!begun){
                uint8_t id[2]={(uint8_t)s_frame_id,(uint8_t)(s_frame_id>>8)};
                if(!batch_append(&batch_size,PROM_FRAME_START,id,sizeof(id)))return;
                begun=true;
            }
            if(!batch_append(&batch_size,type,payload,size))return;
            committed_rows[changed]=(uint8_t)row;
            committed_hashes[changed]=h;
            changed++;
        }
        next_y++;
    }

    /* The final marker can be appended to the same USB submission when space
     * permits. Otherwise it becomes a tiny separate batch on the next wake. */
    bool ending=false;
    if(next_y>=CP_LCD_HEIGHT && begun &&
       sizeof(s_screen_batch)-batch_size>=9u){
        uint8_t id[2]={(uint8_t)s_frame_id,(uint8_t)(s_frame_id>>8)};
        if(!batch_append(&batch_size,PROM_FRAME_END,id,sizeof(id)))return;
        ending=true;
    }
    if(batch_size){
        if(!remote_native_write_screen_batch(s_screen_batch,batch_size)){
            s_screen_backpressure++; /* count failed BATCHES, not rows */
            return;
        }
        s_frame_started=begun;
        for(unsigned i=0;i<changed;i++)
            s_row_hash[committed_rows[i]]=committed_hashes[i];
    }
    s_frame_y=next_y;
    if(ending || (next_y>=CP_LCD_HEIGHT && !begun)){
        s_frame_in_progress=false;
        if(ending)s_frames_completed++;
        if(s_frame_gen==s_snapshot_generation)s_snapshot_ready=false;
    }
}
static void remote_task(void *ignored)
{
    (void)ignored;
    remote_native_set_task(xTaskGetCurrentTaskHandle());
    prom_decoder_t parser={0};prom_packet_t incoming;
    uint8_t rx[1024];
    for(;;){
        /* RX, TX completion and framebuffer publish all wake this task.
         * An actual timeout blocks it; no pdMS_TO_TICKS(1)==0 spin. */
        int bytes=remote_native_read(rx,sizeof(rx));
        if(bytes>0)s_received_bytes+=(uint32_t)bytes;
        for(int i=0;i<bytes;i++)if(prom_decode_byte(&parser,rx[i],&incoming))on_command(&incoming);
        if(!s_active){remote_native_wait(250u);continue;}
        if(!remote_native_connected()||
           (uint32_t)(ticks_ms()-s_last_host_ms)>REM_TIMEOUT_MS){session_off();continue;}
        send_pending_diagnostics(); /* Never let diagnostic TX block a key. */
        if(s_transfer&&!s_upload)download_tick();
        else frame_tick();
        remote_native_wait(20u); /* TX-complete interrupts wake sooner. */
    }
}
esp_err_t remote_usb_init(void)
{
    s_key_queue=xQueueCreate(24,sizeof(rem_key_t));
    s_ack_queue=xQueueCreate(REM_ACK_QUEUE_DEPTH,sizeof(rem_ack_t));
    s_lock=xSemaphoreCreateMutex();
    s_snapshot=heap_caps_malloc(FB_BYTES,MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if(!s_key_queue||!s_ack_queue||!s_lock||!s_snapshot)return ESP_ERR_NO_MEM;
    esp_err_t err=remote_native_init();
    if(err!=ESP_OK)return err;
    if(xTaskCreate(remote_task,"m5_usb_remote",6144,NULL,5,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"USB OTG vendor protocol v1 enabled (requires companion app handshake)");
    return ESP_OK;
}
bool remote_usb_active(void){return s_active;}
void remote_usb_publish(const uint16_t *fb)
{
    if(!s_active||!fb||!s_snapshot||!s_lock)return;
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(15))!=pdTRUE){
        s_publish_lock_misses++;return;
    }
    memcpy(s_snapshot,fb,FB_BYTES);
    s_snapshot_generation++;
    s_snapshot_ready=true;
    xSemaphoreGive(s_lock);
    remote_native_wake();
}
cp_key_event_t remote_usb_poll_key_with_sequence(uint16_t *sequence)
{
    rem_key_t item={{CP_KEY_NONE,0},PROM_KEY_SEQ_LEGACY,0};
    if(s_key_queue)xQueueReceive(s_key_queue,&item,0);
    if(item.event.type!=CP_KEY_NONE){
        s_polled_key_received_ms=item.received_ms;
        s_polled_key_sequence=item.seq;
    }
    if(sequence)*sequence=item.seq;
    return item.event;
}
cp_key_event_t remote_usb_poll_key(void)
{
    return remote_usb_poll_key_with_sequence(NULL);
}
void remote_usb_key_rendered(uint16_t sequence)
{
    s_keys_rendered++;
    /* Key handling is on the UI task, so don't touch TinyUSB here.
       The device-side delay uses its own clock, independent of the host. */
    if(sequence!=PROM_KEY_SEQ_LEGACY && sequence==s_polled_key_sequence)
        ack_key(sequence,PROM_KEY_RENDERED,
                (uint32_t)(ticks_ms()-s_polled_key_received_ms));
}

/*
 * M5 Prom FAT32 microSD storage.
 *
 * Filesystem access is exclusively through the ESP-IDF SDSPI + FatFs VFS
 * (open/mkdir/read/write/rename). There are NO raw-sector writes and the SD
 * card is NEVER formatted. The 256-byte "raw" Xbox EEPROM is only the file
 * payload, not a disk image or filesystem operation.
 */
#include "storage.h"
#include "board.h"
#include "image.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

#ifndef M5_PROM_SD_MOUNT_POINT
#define M5_PROM_SD_MOUNT_POINT "/sdcard"
#endif
#define ROOT M5_PROM_SD_MOUNT_POINT "/EEPROMs"

static const char *TAG="sd_images";
static bool mounted=false, ready=false;
static sdmmc_card_t *sdcard=NULL;
static char last_error[48]="SD NOT INITIALIZED";
static char last_stage[32]="NOT INITIALIZED";
static int last_errno=0;
static char last_hint[48]="INSERT FAT32 SD / REMOUNT";

const char *storage_last_error(void){return last_error;}
const char *storage_last_hint(void){return last_hint;}
bool storage_ready(void){return ready;}
bool storage_mounted(void){return mounted;}
const char *storage_last_stage(void){return last_stage;}
int storage_last_errno(void){return last_errno;}

static void report_ok(const char *stage)
{
    snprintf(last_error,sizeof(last_error),"SD FAT FILES VERIFIED");
    snprintf(last_stage,sizeof(last_stage),"%s",stage);
    last_errno=0;
    last_hint[0]=0;
}
static esp_err_t report_error(const char *stage,int num)
{
    const char *reason="SD FILE ERROR";
    const char *hint="SEE SERIAL LOG / CHECK SD CARD";
    switch(num){
        case EACCES: case EROFS: reason="SD READ-ONLY";hint="CHECK SD WRITE ACCESS";break;
        case ENOSPC: reason="SD CARD FULL";hint="FREE SPACE ON FAT32 CARD";break;
        case EINVAL: case ENAMETOOLONG:
            reason="FAT INVALID / LONG FILE NAME";
            hint="ENABLE FATFS LONG FILENAMES";
            break;
        case ENODEV: reason="SD NOT READY";hint="REINSERT / REMOUNT SD";break;
        case ENFILE: case EMFILE:
            reason="TOO MANY SD FILES OPEN";hint="CLOSE FILE TRANSFERS / RETRY";break;
        case ENOENT:
            reason="SD PATH NOT FOUND";hint="CHECK /EEPROMs FOLDER";break;
        case EIO:
            reason="SD I/O ERROR";hint="CHECK CARD / REINSERT";break;
        default:break;
    }
    snprintf(last_stage,sizeof(last_stage),"%s",stage);
    last_errno=num;
    if(num==EIO||num==ENODEV||num==EROFS||num==EACCES||num==ENOSPC)ready=false;
    snprintf(last_error,sizeof(last_error),"%.16s: %s",stage,reason);
    snprintf(last_hint,sizeof(last_hint),"%s",hint);
    ESP_LOGE(TAG,"%s failed: errno=%d (%s); %s",stage,num,strerror(num),hint);
    return ESP_FAIL;
}
static esp_err_t report_esp(const char *stage,esp_err_t e)
{
    snprintf(last_stage,sizeof(last_stage),"%s",stage);
    last_errno=0;
    ready=false;
    snprintf(last_error,sizeof(last_error),"%.16s: %.26s",stage,esp_err_to_name(e));
    snprintf(last_hint,sizeof(last_hint),"CHECK SERIAL LOG / SD CONNECTION");
    ESP_LOGE(TAG,"%s failed: %s",stage,esp_err_to_name(e));
    return e;
}

/* Read/write smoke test THROUGH THE EXISTING MOUNTED FAT FILESYSTEM.
 * Create a unique temporary LFN file, sync, reopen, verify, then delete it.
 * Do not overwrite user files and never reformat the volume. Also catches
 * an old sdkconfig that accidentally omitted long filename support.
 */
static esp_err_t probe_writable(void)
{
#if defined(CONFIG_FATFS_LFN_NONE)
    snprintf(last_stage,sizeof(last_stage),"SD LFN CONFIG");
    last_errno=0;
    snprintf(last_error,sizeof(last_error),"FAT LONG FILENAMES DISABLED");
    snprintf(last_hint,sizeof(last_hint),"ENABLE FATFS LFN HEAP IN MENUCONFIG");
    ESP_LOGE(TAG,"FATFS_LFN_NONE is enabled in active sdkconfig. Serial folders and .BIN names require long filenames. Enable FATFS_LFN_HEAP in menuconfig and rebuild.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    static const uint8_t token[]={0x4d,0x35,0x50,0x52,0x4f,0x4d,0x2d,0x53,0x44,0x21};
    char path[128]={0};
    int fd=-1;
    for(unsigned n=0;n<100;n++){
        snprintf(path,sizeof(path),"%s/M5PROM_FS_TEST_%02u.TMP",ROOT,n);
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
        if(fd>=0)break;
        if(errno!=EEXIST)return report_error("SD TEST CREATE",errno);
    }
    if(fd<0){
        snprintf(last_stage,sizeof(last_stage),"SD TEST CREATE");
        snprintf(last_error,sizeof(last_error),"SD TEST NAMES EXHAUSTED");
        snprintf(last_hint,sizeof(last_hint),"REMOVE STALE M5PROM_FS_TEST FILES");
        return ESP_ERR_INVALID_STATE;
    }
    bool ok=true;
    int saved_errno=0;
    ssize_t n=write(fd,token,sizeof(token));
    if(n!=(ssize_t)sizeof(token)){ok=false;saved_errno=(n<0)?errno:ENOSPC;}
    if(ok && fsync(fd)!=0){ok=false;saved_errno=errno;}
    if(close(fd)!=0){ok=false;saved_errno=errno;}
    if(ok){
        FILE *f=fopen(path,"rb");
        if(!f){ok=false;saved_errno=errno;}
        else{
            uint8_t check[sizeof(token)]={0};
            size_t got=fread(check,1,sizeof(check),f);
            int extra=fgetc(f);
            if(got!=sizeof(token)||extra!=EOF||memcmp(token,check,sizeof(token))!=0){
                ok=false;saved_errno=EIO;
            }
            if(fclose(f)!=0){ok=false;saved_errno=errno;}
        }
    }
    if(unlink(path)!=0){
        ESP_LOGE(TAG,"Probe cleanup failed for %s: errno=%d",path,errno);
        if(ok){ok=false;saved_errno=errno;}
    }
    return ok?ESP_OK:report_error("SD TEST VERIFY",saved_errno?saved_errno:EIO);
#endif
}

/* Mount once; if a previous writeability test failed, it can be retried
 * without initializing the same SPI host twice. Supports existing FAT32
 * files and directories without altering their contents.
 */
esp_err_t storage_init(void)
{
    if(ready)return ESP_OK;
    if(!mounted){
        const spi_bus_config_t bus={
            .mosi_io_num=CP_SD_MOSI,.miso_io_num=CP_SD_MISO,.sclk_io_num=CP_SD_SCLK,
            .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096
        };
        esp_err_t err=spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO);
        if(err!=ESP_OK)return report_esp("SD SPI INIT",err);
        sdmmc_host_t host=SDSPI_HOST_DEFAULT();
        host.slot=SPI3_HOST;
        host.max_freq_khz=10000; // reliable margin for removable microSD
        sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id=host.slot;slot.gpio_cs=CP_SD_CS;
        esp_vfs_fat_mount_config_t mount={
            .format_if_mount_failed=false, // never erase an existing FAT32 card
            .max_files=8,.allocation_unit_size=16*1024
        };
        err=esp_vfs_fat_sdspi_mount(M5_PROM_SD_MOUNT_POINT,&host,&slot,&mount,&sdcard);
        if(err!=ESP_OK){
            (void)spi_bus_free(SPI3_HOST);
            sdcard=NULL;
            return report_esp("SD FAT MOUNT",err);
        }
        mounted=true;
        ESP_LOGI(TAG,"FAT filesystem mounted on %s without formatting",M5_PROM_SD_MOUNT_POINT);
    }
    if(mkdir(ROOT,0775)!=0 && errno!=EEXIST)
        return report_error("MKDIR EEPROMs",errno);
    struct stat st;
    if(stat(ROOT,&st)!=0)return report_error("CHECK EEPROMs",errno);
    if(!S_ISDIR(st.st_mode)){
        snprintf(last_error,sizeof(last_error),"/EEPROMs IS NOT A DIRECTORY");
        snprintf(last_hint,sizeof(last_hint),"RENAME EXISTING /EEPROMs FILE");
        return ESP_ERR_INVALID_STATE;
    }
    return storage_self_test();
}

esp_err_t storage_self_test(void)
{
    if(!mounted) return storage_init();
    struct stat st;
    if(stat(ROOT,&st)!=0) return report_error("CHECK EEPROMs",errno);
    if(!S_ISDIR(st.st_mode)){
        snprintf(last_stage,sizeof(last_stage),"CHECK EEPROMs");
        snprintf(last_error,sizeof(last_error),"/EEPROMs IS NOT A DIRECTORY");
        snprintf(last_hint,sizeof(last_hint),"RENAME EXISTING /EEPROMs FILE");
        ready=false;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t check=probe_writable();
    ready=(check==ESP_OK);
    if(check!=ESP_OK)return check;
    report_ok("SD SELF TEST");
    ESP_LOGI(TAG,"Existing FAT volume /EEPROMs write/sync/readback/delete PASS");
    return ESP_OK;
}

esp_err_t storage_remount(void)
{
    ready=false;
    if(mounted){
        esp_err_t err=esp_vfs_fat_sdcard_unmount(M5_PROM_SD_MOUNT_POINT,sdcard);
        if(err!=ESP_OK)return report_esp("SD UNMOUNT",err);
        mounted=false;sdcard=NULL;
        err=spi_bus_free(SPI3_HOST);
        if(err!=ESP_OK)return report_esp("SD SPI FREE",err);
    }
    return storage_init();
}

static bool valid_part(const char *name)
{
    if(!name||!name[0]||strlen(name)>=STORAGE_NAME_SIZE||strstr(name,".."))return false;
    for(const char *p=name;*p;p++){
        if(!(isalnum((unsigned char)*p)||*p=='_'||*p=='-'||*p=='.'))return false;
    }
    return true;
}
static bool is_folder(const char *path)
{
    struct stat st;
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
}
static esp_err_t ensure_folder(const char *folder)
{
    if(!valid_part(folder)){
        snprintf(last_error,sizeof(last_error),"INVALID SERIAL FOLDER NAME");
        return ESP_ERR_INVALID_ARG;
    }
    if(mkdir(ROOT,0775)!=0&&errno!=EEXIST)return report_error("MKDIR EEPROMs",errno);
    char path[128];snprintf(path,sizeof(path),"%s/%s",ROOT,folder);
    if(mkdir(path,0775)!=0&&errno!=EEXIST)return report_error("MKDIR SERIAL",errno);
    if(!is_folder(path)){
        snprintf(last_error,sizeof(last_error),"SERIAL PATH NOT A DIRECTORY");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
static uint32_t crc32(const uint8_t data[256])
{
    uint32_t c=0xffffffffu;
    for(int i=0;i<256;i++){
        c^=data[i];for(int j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320u:0);
    }
    return ~c;
}
esp_err_t storage_folder_for_raw(const uint8_t raw[256],char *folder,size_t len)
{
    if(!ready||!raw||!folder)return ESP_ERR_INVALID_STATE;
    if(image_serial_valid(raw)){
        if(len<13)return ESP_ERR_INVALID_SIZE;
        memcpy(folder,raw+0x34,12);folder[12]=0;
    }else{
        if(len<17)return ESP_ERR_INVALID_SIZE;
        snprintf(folder,len,"UNKNOWN_%08lX",(unsigned long)crc32(raw));
    }
    return ensure_folder(folder);
}
static bool correct_bin(const char *filename)
{
    size_t n=strlen(filename);
    return n>4 && !strcasecmp(filename+n-4,".BIN");
}
static bool verify_saved(const char *path,const uint8_t raw[256])
{
    FILE *f=fopen(path,"rb");if(!f)return false;
    uint8_t check[256];size_t n=fread(check,1,256,f);
    int extra=fgetc(f);int closed=fclose(f);
    return n==256&&extra==EOF&&closed==0&&memcmp(raw,check,256)==0;
}
esp_err_t storage_save_version(const char *folder,const char *tag,const uint8_t raw[256],char *name,size_t name_len)
{
    if(!ready||!folder||!tag||!raw||!name||name_len<20)return ESP_ERR_INVALID_STATE;
    if(!valid_part(tag)||strlen(tag)>STORAGE_NAME_SIZE-10)return ESP_ERR_INVALID_ARG;
    esp_err_t e=ensure_folder(folder);
    if(e!=ESP_OK)return e;
    for(unsigned id=1;id<=9999;id++){
        char filename[STORAGE_NAME_SIZE];char path[160];
        snprintf(filename,sizeof(filename),"%s_%04u.BIN",tag,id);
        snprintf(path,sizeof(path),"%s/%s/%s",ROOT,folder,filename);
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
        if(fd<0){
            if(errno==EEXIST)continue;
            return report_error("CREATE EEPROM",errno);
        }
        size_t written=0;bool success=true;
        int fail_errno=0;
        while(written<256){
            ssize_t n=write(fd,raw+written,256-written);
            if(n<=0){success=false;fail_errno=n<0?errno:ENOSPC;break;}
            written+=(size_t)n;
        }
        if(success&&fsync(fd)!=0){success=false;fail_errno=errno;}
        if(close(fd)!=0){success=false;fail_errno=errno;}
        if(!success){
            (void)unlink(path);
            return report_error("WRITE EEPROM",fail_errno?fail_errno:EIO);
        }
        if(!verify_saved(path,raw)){
            (void)unlink(path);
            snprintf(last_stage,sizeof(last_stage),"EEPROM SAVE VERIFY");
            last_errno=EIO;
            ready=false;
            snprintf(last_error,sizeof(last_error),"EEPROM FILE VERIFY FAILED");
            snprintf(last_hint,sizeof(last_hint),"RETRY WITH ANOTHER SD CARD");
            ESP_LOGE(TAG,"256-byte EEPROM verification failed: %s",path);
            return ESP_ERR_INVALID_RESPONSE;
        }
        snprintf(name,name_len,"%s",filename);
        report_ok("EEPROM SAVE VERIFIED");
        ESP_LOGI(TAG,"Verified FAT file /EEPROMs/%s/%s (256 bytes)",folder,filename);
        return ESP_OK;
    }
    snprintf(last_error,sizeof(last_error),"EEPROM FILE NUMBERS EXHAUSTED");
    return ESP_ERR_NO_MEM;
}
esp_err_t storage_load_version(const char *folder,const char *name,uint8_t raw[256])
{
    if(!ready||!valid_part(folder)||!valid_part(name)||!correct_bin(name)||!raw)
        return ESP_ERR_INVALID_ARG;
    char path[160];snprintf(path,sizeof(path),"%s/%s/%s",ROOT,folder,name);
    FILE *f=fopen(path,"rb");if(!f)return report_error("OPEN EEPROM",errno);
    size_t n=fread(raw,1,256,f);int extra=fgetc(f);int closed=fclose(f);
    if(n!=256||extra!=EOF||closed!=0){
        snprintf(last_error,sizeof(last_error),"EEPROM FILE NOT 256 BYTES");
        snprintf(last_hint,sizeof(last_hint),"SELECT A VALID EEPROM BIN");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
static int names_compare(const void *a,const void *b){return strcasecmp((const char*)a,(const char*)b);}
static size_t scan(const char *folder,bool folders,char names[][STORAGE_NAME_SIZE],size_t max)
{
    if(!ready||!names||!max)return 0;
    char path[128];
    if(folder){if(!valid_part(folder))return 0;snprintf(path,sizeof(path),"%s/%s",ROOT,folder);}
    else snprintf(path,sizeof(path),"%s",ROOT);
    DIR *d=opendir(path);if(!d)return 0;
    size_t n=0;struct dirent *ent;
    while((ent=readdir(d))!=NULL&&n<max){
        if(!valid_part(ent->d_name))continue;
        size_t plen=strlen(path),nlen=strlen(ent->d_name);
        char entry_path[160];
        if(plen+1+nlen>=sizeof(entry_path))continue;
        memcpy(entry_path,path,plen);entry_path[plen]='/';
        memcpy(entry_path+plen+1,ent->d_name,nlen+1);
        bool isdir=is_folder(entry_path);
        if(isdir!=folders)continue;
        if(!folders&&!correct_bin(ent->d_name))continue;
        memcpy(names[n++],ent->d_name,nlen+1);
    }
    closedir(d);
    qsort(names,n,STORAGE_NAME_SIZE,names_compare);
    return n;
}
size_t storage_scan_folders(char names[][STORAGE_NAME_SIZE],size_t maximum)
{return scan(NULL,true,names,maximum);}
size_t storage_scan_files(const char *folder,char names[][STORAGE_NAME_SIZE],size_t maximum)
{return scan(folder,false,names,maximum);}

/* Plain-text HDD key export through the mounted FAT VFS only. Reports are
 * deliberately versioned: no prior TXT or EEPROM backup is overwritten.
 * The caller holds operation_guard, so web uploads cannot race this write.
 */
esp_err_t storage_save_hdd_report(const char *folder,const char *report,size_t report_len,
                                  char *name,size_t name_len)
{
    if(name&&name_len)name[0]=0;
    if(!ready||!folder||!report||!name)return ESP_ERR_INVALID_STATE;
    if(name_len<STORAGE_NAME_SIZE)return ESP_ERR_INVALID_ARG;
    if(report_len==0||report_len>1024||strlen(report)!=report_len)return ESP_ERR_INVALID_ARG;
    esp_err_t e=ensure_folder(folder);
    if(e!=ESP_OK)return e;
    for(unsigned id=1;id<=9999;id++){
        char filename[STORAGE_NAME_SIZE],path[160];
        snprintf(filename,sizeof(filename),"HDDKEY_%04u.TXT",id);
        snprintf(path,sizeof(path),"%s/%s/%s",ROOT,folder,filename);
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
        if(fd<0){
            if(errno==EEXIST)continue;
            return report_error("HDD TXT CREATE",errno);
        }
        bool success=true;
        int saved_errno=0;
        size_t done=0;
        while(done<report_len){
            ssize_t n=write(fd,report+done,report_len-done);
            if(n<=0){success=false;saved_errno=n<0?errno:ENOSPC;break;}
            done+=(size_t)n;
        }
        if(success&&fsync(fd)!=0){success=false;saved_errno=errno;}
        if(close(fd)!=0){success=false;saved_errno=errno;}
        if(!success){
            (void)unlink(path);
            return report_error("HDD TXT WRITE",saved_errno?saved_errno:EIO);
        }
        FILE *f=fopen(path,"rb");
        if(!f){
            saved_errno=errno;
            (void)unlink(path);
            return report_error("HDD TXT READBACK",saved_errno);
        }
        unsigned char buf[128];
        size_t checked=0;
        while(checked<report_len){
            const size_t remaining=report_len-checked;
            const size_t chunk=remaining<sizeof(buf)?remaining:sizeof(buf);
            const size_t got=fread(buf,1,chunk,f);
            if(got!=chunk||memcmp(buf,report+checked,chunk)!=0){success=false;break;}
            checked+=chunk;
        }
        if(success&&fgetc(f)!=EOF)success=false;
        if(fclose(f)!=0)success=false;
        memset(buf,0,sizeof(buf));
        if(!success){
            (void)unlink(path);
            return report_error("HDD TXT VERIFY",EIO);
        }
        snprintf(name,name_len,"%s",filename);
        report_ok("HDD TXT VERIFIED");
        ESP_LOGI(TAG,"Saved and verified HDD recovery TXT (%u bytes)", (unsigned)report_len);
        return ESP_OK;
    }
    snprintf(last_stage,sizeof(last_stage),"HDD TXT CREATE");
    snprintf(last_error,sizeof(last_error),"HDD TXT NUMBERS EXHAUSTED");
    snprintf(last_hint,sizeof(last_hint),"REMOVE OLD HDDKEY FILES");
    return ESP_ERR_NO_MEM;
}

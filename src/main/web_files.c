/*
 * Full microSD browser for M5 Prom: list / create / upload / download /
 * rename / delete files and folders. No protected names or folder ACLs.
 * Intentional operational scope is the /sdcard FAT mount, NOT SPI flash/NVS.
 * Wi-Fi must be private. HTTP is not end-to-end encrypted.
 */
#include "web_files.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "storage.h"
#include "operation_guard.h"

static const char *TAG="m5_webfiles";
static httpd_handle_t s_server;
#ifndef M5_PROM_SD_MOUNT_POINT
#define M5_PROM_SD_MOUNT_POINT "/sdcard"
#endif
#define SD_BASE M5_PROM_SD_MOUNT_POINT
#define MAX_PATH 240
#define MAX_UPLOAD (16*1024*1024)
#define IO_CHUNK 1024
extern const unsigned char web_html_start[] __asm__("_binary_index_html_start");
extern const unsigned char web_html_end[] __asm__("_binary_index_html_end");

static esp_err_t reject(httpd_req_t *req,httpd_err_code_t code,const char *message)
{return httpd_resp_send_err(req,code,message);}
static esp_err_t conflict(httpd_req_t *req,const char *message)
{
    httpd_resp_set_status(req,"409 Conflict");
    httpd_resp_set_type(req,"text/plain");
    return httpd_resp_sendstr(req,message);
}
static bool query_value(httpd_req_t *req,const char *key,char *out,size_t len)
{
    char query[600];
    if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK)return false;
    char encoded[512];
    if(httpd_query_key_value(query,key,encoded,sizeof(encoded))!=ESP_OK)return false;
    size_t index=0;
    for(const char *p=encoded;*p;p++){
        unsigned char c=(unsigned char)*p;
        if(c=='%'){
            if(!isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2]))return false;
            unsigned hi=(unsigned)(isdigit((unsigned char)p[1])?p[1]-'0':10+tolower((unsigned char)p[1])-'a');
            unsigned lo=(unsigned)(isdigit((unsigned char)p[2])?p[2]-'0':10+tolower((unsigned char)p[2])-'a');
            c=(unsigned char)((hi<<4)|lo);
            p+=2;
        }
        // Query paths use encodeURIComponent; do not translate '+' to space.
        if(index+1>=len||c==0)return false;
        out[index++]=(char)c;
    }
    out[index]=0;
    return true;
}
static bool is_safe_path(const char *relative,char full[MAX_PATH])
{
    // Absolute-looking paths below are web-virtual, not OS-rooted.
    if(!relative||relative[0]!='/')return false;
    const size_t n=strlen(relative);
    if(n>MAX_PATH-strlen(SD_BASE)-2)return false;
    if(strchr(relative,'\\'))return false;
    const char *p=relative+1;
    while(*p){
        const char *end=strchr(p,'/');
        size_t len=end?(size_t)(end-p):strlen(p);
        if(!len || (len==1&&p[0]=='.') || (len==2&&p[0]=='.'&&p[1]=='.'))return false;
        for(size_t i=0;i<len;i++){
            unsigned char c=(unsigned char)p[i];
            if(c<32||c==127||c==':'||c=='*'||c=='?'||c=='"'||c=='|'||c=='<'||c=='>')return false;
        }
        p=end?end+1:p+len;
    }
    int written=snprintf(full,MAX_PATH,"%s%s",SD_BASE,relative);
    return written>0&&written<MAX_PATH;
}
static bool get_path(httpd_req_t *req,const char *key,char path[MAX_PATH],char *relative,size_t relative_max)
{
    if(!query_value(req,key,relative,relative_max))return false;
    return is_safe_path(relative,path);
}
static bool exists(const char *path,struct stat *st){return stat(path,st)==0;}
static bool mutation_allowed(httpd_req_t *req)
{
    // Simple custom-header barrier against cross-site forms; no login/ACL.
    char value[8];
    return httpd_req_get_hdr_value_str(req,"X-M5Prom",value,sizeof(value))==ESP_OK&&
           strcmp(value,"1")==0;
}
static esp_err_t html_get(httpd_req_t *req)
{
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,(const char*)web_html_start,
                           (ssize_t)(web_html_end-web_html_start));
}
static void json_escaped(const char *input,char *out,size_t cap)
{
    size_t n=0;
    for(const unsigned char *p=(const unsigned char*)input;*p&&n+8<cap;p++){
        unsigned char c=*p;
        if(c=='"'||c=='\\'){out[n++]='\\';out[n++]=(char)c;}
        else if(c<32){(void)snprintf(out+n,cap-n,"\\u%04x",(unsigned)c);n+=6;}
        else out[n++]=(char)c;
    }
    out[n]=0;
}
static esp_err_t list_get(httpd_req_t *req)
{
    char path[MAX_PATH],relative[200];
    if(!get_path(req,"path",path,relative,sizeof(relative)))return reject(req,HTTPD_400_BAD_REQUEST,"Invalid path");
    DIR *dir=opendir(path);
    if(!dir)return reject(req,HTTPD_404_NOT_FOUND,"Directory unavailable; SD mounted?");
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    char escaped[410],line[1024];
    json_escaped(relative,escaped,sizeof(escaped));
    snprintf(line,sizeof(line),"{\"path\":\"%s\",\"entries\":[",escaped);
    esp_err_t err=httpd_resp_sendstr_chunk(req,line);
    bool first=true;
    unsigned count=0;
    struct dirent *entry;
    while(err==ESP_OK&&(entry=readdir(dir))!=NULL && count<400){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        const size_t plen=strlen(path),elen=strlen(entry->d_name);
        char item[MAX_PATH];
        if(plen+elen+2>=sizeof(item))continue;
        memcpy(item,path,plen);
        item[plen]='/';
        memcpy(item+plen+1,entry->d_name,elen+1);
        struct stat st;
        if(!exists(item,&st))continue;
        json_escaped(entry->d_name,escaped,sizeof(escaped));
        snprintf(line,sizeof(line),"%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lu}",
                 first?"":",",escaped,S_ISDIR(st.st_mode)?"true":"false",(unsigned long)st.st_size);
        err=httpd_resp_sendstr_chunk(req,line);
        first=false;count++;
    }
    closedir(dir);
    if(err==ESP_OK)err=httpd_resp_sendstr_chunk(req,"]}");
    if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);
    return err;
}
static esp_err_t download_get(httpd_req_t *req)
{
    char path[MAX_PATH],rel[200];
    if(!get_path(req,"path",path,rel,sizeof(rel)))return reject(req,HTTPD_400_BAD_REQUEST,"Invalid path");
    struct stat st;
    if(!exists(path,&st)||!S_ISREG(st.st_mode))return reject(req,HTTPD_404_NOT_FOUND,"File not found");
    FILE *fp=fopen(path,"rb");
    if(!fp)return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot open file");
    httpd_resp_set_type(req,"application/octet-stream");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    const char *base=strrchr(rel,'/');base=base?base+1:rel;
    // FAT names may contain punctuation; Content-Disposition filename is kept
    // simple, and the browser also uses a download attribute.
    char disposition[300];
    if(strchr(base,'"')||strchr(base,'\r')||strchr(base,'\n'))base="download.bin";
    snprintf(disposition,sizeof(disposition),"attachment; filename=\"%.200s\"",base);
    httpd_resp_set_hdr(req,"Content-Disposition",disposition);
    char block[IO_CHUNK];
    size_t n;
    esp_err_t err=ESP_OK;
    while((n=fread(block,1,sizeof(block),fp))>0){
        err=httpd_resp_send_chunk(req,block,(ssize_t)n);
        if(err!=ESP_OK)break;
    }
    if(ferror(fp))err=ESP_FAIL;
    if(fclose(fp)!=0)err=ESP_FAIL;
    if(err==ESP_OK)err=httpd_resp_send_chunk(req,NULL,0);
    return err;
}
static uint32_t crc_step(uint32_t crc,const uint8_t *data,size_t n)
{
    for(size_t i=0;i<n;i++){
        crc^=data[i];
        for(int b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xEDB88320u:0);
    }
    return crc;
}
static esp_err_t upload_post(httpd_req_t *req)
{
    if(!mutation_allowed(req))return reject(req,HTTPD_403_FORBIDDEN,"Web UI required");
    char path[MAX_PATH],rel[200];
    if(!get_path(req,"path",path,rel,sizeof(rel))||!strcmp(rel,"/"))
        return reject(req,HTTPD_400_BAD_REQUEST,"Invalid file path");
    if(req->content_len<=0||req->content_len>MAX_UPLOAD)
        return reject(req,HTTPD_400_BAD_REQUEST,"Upload must be 1-16 MiB or smaller");
    struct stat st;
    if(exists(path,&st))return conflict(req,"Name already exists; rename or delete first");
    char tmp[MAX_PATH];
    if(snprintf(tmp,sizeof(tmp),"%s.m5upload",path)>=(int)sizeof(tmp))
        return reject(req,HTTPD_400_BAD_REQUEST,"File name too long");
    int fd=open(tmp,O_CREAT|O_EXCL|O_WRONLY,0644);
    if(fd<0)return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot create temporary upload");
    uint8_t data[IO_CHUNK];
    int remaining=req->content_len;
    int consecutive_timeouts=0;
    bool ok=true;
    uint32_t crc=0xFFFFFFFFu;
    while(remaining>0){
        int want=remaining<(int)sizeof(data)?remaining:(int)sizeof(data);
        int n=httpd_req_recv(req,(char*)data,want);
        if(n==HTTPD_SOCK_ERR_TIMEOUT){
            // Release the SD operation gate if a client vanishes mid-upload.
            // HTTPD wait timeout is configured to 8 s; do not retry forever.
            if(++consecutive_timeouts>=3){ok=false;break;}
            continue;
        }
        if(n<=0){ok=false;break;}
        consecutive_timeouts=0;
        size_t pos=0;
        while(pos<(size_t)n){
            ssize_t w=write(fd,data+pos,(size_t)n-pos);
            if(w<=0){ok=false;break;}
            pos+=(size_t)w;
        }
        if(!ok)break;
        crc=crc_step(crc,data,(size_t)n);
        remaining-=n;
    }
    if(fsync(fd)!=0)ok=false;
    if(close(fd)!=0)ok=false;
    if(ok&&remaining==0){
        FILE *fp=fopen(tmp,"rb");
        if(!fp)ok=false;
        else {
            uint32_t check=0xFFFFFFFFu;
            size_t total=0;
            size_t n;
            while((n=fread(data,1,sizeof(data),fp))>0){
                check=crc_step(check,data,n);
                total+=(int)n;
            }
            if(ferror(fp)||total!=req->content_len||check!=crc)ok=false;
            if(fclose(fp)!=0)ok=false;
        }
    }
    if(ok && !exists(path,&st) && rename(tmp,path)==0){
        httpd_resp_set_type(req,"text/plain");
        return httpd_resp_sendstr(req,"Uploaded and CRC-verified");
    }
    unlink(tmp);
    return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Upload failed or verify mismatch");
}
static bool delete_tree(const char *path,int depth)
{
    if(depth>12)return false;
    struct stat st;
    if(!exists(path,&st))return false;
    if(!S_ISDIR(st.st_mode))return unlink(path)==0;
    DIR *d=opendir(path);
    if(!d)return false;
    bool ok=true;
    struct dirent *item;
    while((item=readdir(d))!=NULL){
        if(!strcmp(item->d_name,".")||!strcmp(item->d_name,".."))continue;
        char child[MAX_PATH];
        size_t prefix_len=strlen(path),name_len=strlen(item->d_name);
        if(prefix_len+name_len+2>=sizeof(child)){ok=false;break;}
        memcpy(child,path,prefix_len);
        child[prefix_len]='/';
        memcpy(child+prefix_len+1,item->d_name,name_len+1);
        if(!delete_tree(child,depth+1)){ok=false;break;}
    }
    closedir(d);
    return ok&&rmdir(path)==0;
}
static esp_err_t delete_post(httpd_req_t *req)
{
    if(!mutation_allowed(req))return reject(req,HTTPD_403_FORBIDDEN,"Web UI required");
    char path[MAX_PATH],rel[200];
    if(!get_path(req,"path",path,rel,sizeof(rel))||!strcmp(rel,"/"))
        return reject(req,HTTPD_400_BAD_REQUEST,"Root cannot be deleted");
    if(!delete_tree(path,0))return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Delete failed");
    return httpd_resp_sendstr(req,"Deleted");
}
static esp_err_t mkdir_post(httpd_req_t *req)
{
    if(!mutation_allowed(req))return reject(req,HTTPD_403_FORBIDDEN,"Web UI required");
    char path[MAX_PATH],rel[200];
    if(!get_path(req,"path",path,rel,sizeof(rel))||!strcmp(rel,"/"))
        return reject(req,HTTPD_400_BAD_REQUEST,"Invalid folder");
    struct stat st;
    if(exists(path,&st))return conflict(req,"Folder or file already exists");
    if(mkdir(path,0775)!=0)return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cannot create folder");
    return httpd_resp_sendstr(req,"Folder created");
}
static esp_err_t rename_post(httpd_req_t *req)
{
    if(!mutation_allowed(req))return reject(req,HTTPD_403_FORBIDDEN,"Web UI required");
    char src[MAX_PATH],dst[MAX_PATH],rel_a[200],rel_b[200];
    if(!get_path(req,"from",src,rel_a,sizeof(rel_a)) ||
       !get_path(req,"to",dst,rel_b,sizeof(rel_b)) ||
       !strcmp(rel_a,"/")||!strcmp(rel_b,"/"))
        return reject(req,HTTPD_400_BAD_REQUEST,"Invalid rename");
    struct stat st;
    if(!exists(src,&st))return reject(req,HTTPD_404_NOT_FOUND,"Source not found");
    if(exists(dst,&st))return conflict(req,"Destination already exists");
    if(rename(src,dst)!=0)return reject(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Rename failed");
    return httpd_resp_sendstr(req,"Renamed");
}
/* Serialize every SD filesystem endpoint with foreground backups,
 * PREWRITE confirmation and physical EEPROM programming. HTML itself needs
 * no file access; downloads/listings are guarded as well as mutations.
 */
static esp_err_t guarded_request(httpd_req_t *req, esp_err_t (*fn)(httpd_req_t *))
{
    if(!operation_guard_try_acquire())
        return conflict(req,"M5 Prom busy: EEPROM or SD operation in progress. Retry later.");
    if(!storage_ready()){
        operation_guard_release();
        return conflict(req,"SD is not ready; check Settings > Diagnostics and Remount SD.");
    }
    esp_err_t result=fn(req);
    operation_guard_release();
    return result;
}
#define DEFINE_GUARDED(name) \
    static esp_err_t guarded_##name(httpd_req_t *req){return guarded_request(req,name);}
DEFINE_GUARDED(list_get)
DEFINE_GUARDED(download_get)
DEFINE_GUARDED(upload_post)
DEFINE_GUARDED(delete_post)
DEFINE_GUARDED(mkdir_post)
DEFINE_GUARDED(rename_post)
#undef DEFINE_GUARDED

void web_files_tick(bool enabled,bool network_up)
{
    const bool should_run=enabled&&network_up;
    if(should_run==!!s_server)return;
    if(!should_run){
        httpd_stop(s_server);s_server=NULL;
        ESP_LOGI(TAG,"HTTP file manager stopped");
        return;
    }
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    config.stack_size=8192;
    config.max_uri_handlers=8;
    config.max_open_sockets=3;
    config.recv_wait_timeout=8;
    config.send_wait_timeout=8;
    if(httpd_start(&s_server,&config)!=ESP_OK){
        s_server=NULL;
        ESP_LOGW(TAG,"Unable to start file manager");
        return;
    }
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=html_get},
        {.uri="/api/list",.method=HTTP_GET,.handler=guarded_list_get},
        {.uri="/api/download",.method=HTTP_GET,.handler=guarded_download_get},
        {.uri="/api/upload",.method=HTTP_POST,.handler=guarded_upload_post},
        {.uri="/api/delete",.method=HTTP_POST,.handler=guarded_delete_post},
        {.uri="/api/mkdir",.method=HTTP_POST,.handler=guarded_mkdir_post},
        {.uri="/api/rename",.method=HTTP_POST,.handler=guarded_rename_post}
    };
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++){
        if(httpd_register_uri_handler(s_server,&routes[i])!=ESP_OK){
            httpd_stop(s_server);s_server=NULL;
            ESP_LOGE(TAG,"Cannot register web handler");return;
        }
    }
    ESP_LOGI(TAG,"File manager serving full SD filesystem on private LAN port 80");
}
bool web_files_running(void){return s_server!=NULL;}

/* M5 Prom read/write FAT32 SD file manager. No raw sectors or special permissions. */
#include "sd_browser.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef M5_PROM_SD_MOUNT_POINT
#define M5_PROM_SD_MOUNT_POINT "/sdcard"
#endif
#define SD_PATH_SIZE (sizeof(M5_PROM_SD_MOUNT_POINT) + SD_BROWSER_DIR_MAX + SD_BROWSER_NAME_MAX + 8)
#define MAX_RECURSION 16

static char last_error[96] = "SD READY";
const char *sd_browser_error(void) { return last_error; }

static esp_err_t sd_error(const char *stage, int e)
{
    snprintf(last_error, sizeof(last_error), "%.31s: %.55s", stage, strerror(e ? e : EIO));
    return ESP_FAIL;
}
static esp_err_t sd_error_arg(const char *message)
{
    snprintf(last_error, sizeof(last_error), "%s", message);
    return ESP_ERR_INVALID_ARG;
}
static bool sd_available(void)
{
    if (storage_mounted()) return true;
    snprintf(last_error,sizeof(last_error),"SD NOT MOUNTED - REMOUNT IN SETTINGS");
    return false;
}
/* FAT short names and VFAT long names are both accepted, including spaces.
 * Reject traversal, separators and reserved DOS characters. An existing
 * Unicode name can be browsed even if the 5x7 font cannot display its glyphs.
 */
bool sd_browser_name_valid(const char *s)
{
    if (!s || !*s || !strcmp(s,".") || !strcmp(s,"..")) return false;
    size_t n = strlen(s);
    if (n >= SD_BROWSER_NAME_MAX || s[n-1] == ' ' || s[n-1] == '.') return false;
    for (const unsigned char *p=(const unsigned char*)s; *p; ++p)
        if (*p < 32 || *p==127 || strchr("/\\:*?\"<>|", *p)) return false;
    /* Windows reserved device names also cause problems on FAT volumes. */
    char base[16]={0}; size_t i=0;
    while (i<n && i<sizeof(base)-1 && s[i]!='.') {base[i]=(char)toupper((unsigned char)s[i]); ++i;}
    base[i]=0;
    if (!strcmp(base,"CON") || !strcmp(base,"PRN") || !strcmp(base,"AUX") ||
        !strcmp(base,"NUL") || ((strlen(base)==4) &&
        ((strncmp(base,"COM",3)==0 || strncmp(base,"LPT",3)==0) &&
          base[3]>='1' && base[3]<='9'))) return false;
    return true;
}
static bool valid_dir(const char *dir)
{
    if (!dir || strlen(dir) >= SD_BROWSER_DIR_MAX || dir[0]=='/') return false;
    if (!*dir) return true;
    const char *p=dir;
    while (*p) {
        const char *end=strchr(p,'/');
        size_t n=end?(size_t)(end-p):strlen(p);
        if (!n || n>=SD_BROWSER_NAME_MAX) return false;
        char part[SD_BROWSER_NAME_MAX];
        memcpy(part,p,n);part[n]=0;
        if (!sd_browser_name_valid(part)) return false;
        if (!end) break;
        p=end+1;
    }
    return true;
}
static bool make_path(char *out, size_t cap, const char *dir, const char *name)
{
    if (!valid_dir(dir) || (name && !sd_browser_name_valid(name))) return false;
    int n;
    if (dir[0]) n=name ? snprintf(out,cap,"%s/%s/%s",M5_PROM_SD_MOUNT_POINT,dir,name)
                        : snprintf(out,cap,"%s/%s",M5_PROM_SD_MOUNT_POINT,dir);
    else n=name ? snprintf(out,cap,"%s/%s",M5_PROM_SD_MOUNT_POINT,name)
                : snprintf(out,cap,"%s",M5_PROM_SD_MOUNT_POINT);
    return n>0 && (size_t)n<cap;
}
static bool subdir(char *out,size_t cap,const char *dir,const char *name)
{
    if (!valid_dir(dir) || !sd_browser_name_valid(name)) return false;
    int n=dir[0]?snprintf(out,cap,"%s/%s",dir,name):snprintf(out,cap,"%s",name);
    return n>0&&(size_t)n<cap;
}
static esp_err_t existing_regular(const char *path, struct stat *st)
{
    if (stat(path,st)!=0) return sd_error("SD LOOKUP",errno);
    if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode))
        return sd_error_arg("UNSUPPORTED FILE TYPE");
    return ESP_OK;
}
esp_err_t sd_browser_list(const char *dir,size_t offset,sd_browser_entry_t *page,
                          size_t capacity,size_t *page_count,size_t *total)
{
    if (!page_count || !total || (capacity && !page)) return ESP_ERR_INVALID_ARG;
    *page_count=0;*total=0;
    if (!sd_available()) return ESP_ERR_INVALID_STATE;
    /* This runs in app_main's limited FreeRTOS stack. Previously two large
     * SD_PATH_SIZE pathname arrays were on that stack at once; entering a
     * folder adds the caller's pathname frame too. Use bounded heap scratch. */
    struct listing_paths {char dirpath[SD_PATH_SIZE],itempath[SD_PATH_SIZE];};
    struct listing_paths *paths=malloc(sizeof(*paths));
    if(!paths)return sd_error("SD LIST MEMORY",ENOMEM);
    if(!make_path(paths->dirpath,sizeof(paths->dirpath),dir,NULL)){
        free(paths);return sd_error_arg("INVALID FOLDER PATH");
    }
    DIR *d=opendir(paths->dirpath);
    if(!d){int e=errno;free(paths);return sd_error("SD LIST",e);}
    struct dirent *ent;
    int err=0;
    /* errno from stat() must never masquerade as a readdir() error. */
    for(;;) {
        errno=0;
        ent=readdir(d);
        if(!ent){err=errno;break;}
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
        if (!sd_browser_name_valid(ent->d_name)) continue;
        struct stat st;
        if (!make_path(paths->itempath,sizeof(paths->itempath),dir,ent->d_name)) continue;
        if (stat(paths->itempath,&st)!=0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        if (*total>=offset && *page_count<capacity) {
            sd_browser_entry_t *v=&page[*page_count];
            memset(v,0,sizeof(*v));
            snprintf(v->name,sizeof(v->name),"%s",ent->d_name);
            v->directory=S_ISDIR(st.st_mode);
            v->size=v->directory?0:(uint64_t)st.st_size;
            ++*page_count;
        }
        ++*total;
    }
    if(closedir(d)!=0 && !err)err=errno;
    free(paths);
    if(err)return sd_error("SD LIST READ",err);
    snprintf(last_error,sizeof(last_error),"SD FILESYSTEM READY");
    return ESP_OK;
}
esp_err_t sd_browser_mkdir(const char *dir,const char *name)
{
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    char *path=malloc(SD_PATH_SIZE);
    if(!path)return sd_error("MKDIR MEMORY",ENOMEM);
    if(!make_path(path,SD_PATH_SIZE,dir,name)){
        free(path);return sd_error_arg("INVALID NEW FOLDER NAME");
    }
    int rc=mkdir(path,0775),saved=errno;
    free(path);
    if(rc!=0)return sd_error("SD MKDIR",saved);
    snprintf(last_error,sizeof(last_error),"FOLDER CREATED");return ESP_OK;
}
esp_err_t sd_browser_rename(const char *dir,const char *old_name,const char *new_name)
{
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    /* A pair of full SD paths would use ~1.5 KB of the main task stack. */
    struct rename_paths {char src[SD_PATH_SIZE],dst[SD_PATH_SIZE];};
    struct rename_paths *paths=malloc(sizeof(*paths));
    if(!paths)return sd_error("RENAME MEMORY",ENOMEM);
    esp_err_t result=ESP_FAIL;
    struct stat st;
    if(!make_path(paths->src,sizeof(paths->src),dir,old_name)||
       !make_path(paths->dst,sizeof(paths->dst),dir,new_name)){
        result=sd_error_arg("INVALID NAME");goto done;
    }
    if(!strcmp(old_name,new_name)){result=ESP_OK;goto done;}
    result=existing_regular(paths->src,&st);
    if(result!=ESP_OK)goto done;
    if(stat(paths->dst,&st)==0){result=sd_error("NAME ALREADY USED",EEXIST);goto done;}
    if(errno!=ENOENT){result=sd_error("NAME CHECK",errno);goto done;}
    if(rename(paths->src,paths->dst)!=0){result=sd_error("SD RENAME",errno);goto done;}
    snprintf(last_error,sizeof(last_error),"ITEM RENAMED");result=ESP_OK;
done:
    free(paths);
    return result;
}
static esp_err_t copy_file_verified(const char *src,const char *dst)
{
    /* Keep the streaming + readback buffers off the Cardputer main stack. */
    struct copy_buffers {unsigned char read[512],verify[512];};
    struct copy_buffers *work=malloc(sizeof(*work));
    if(!work)return sd_error("COPY MEMORY",ENOMEM);
    FILE *from=fopen(src,"rb");
    if(!from){int e=errno;free(work);return sd_error("COPY SOURCE",e);}
    int out=open(dst,O_WRONLY|O_CREAT|O_EXCL,0644);
    if(out<0){int e=errno;fclose(from);free(work);return sd_error("COPY DEST",e);}
    esp_err_t result=ESP_OK;int saved_errno=0;
    for(;;){
        size_t n=fread(work->read,1,sizeof(work->read),from);
        if(n){size_t done=0;while(done<n){
            ssize_t w=write(out,work->read+done,n-done);
            if(w<=0){saved_errno=w<0?errno:ENOSPC;result=ESP_FAIL;break;}
            done+=(size_t)w;
        }}
        if(result!=ESP_OK)break;
        if(n<sizeof(work->read)){
            if(ferror(from)){result=ESP_FAIL;saved_errno=EIO;}
            break;
        }
    }
    if(result==ESP_OK && fsync(out)!=0){result=ESP_FAIL;saved_errno=errno;}
    if(close(out)!=0 && result==ESP_OK){result=ESP_FAIL;saved_errno=errno;}
    if(fclose(from)!=0 && result==ESP_OK){result=ESP_FAIL;saved_errno=EIO;}
    if(result==ESP_OK){
        FILE *a=fopen(src,"rb"),*b=fopen(dst,"rb");
        if(!a || !b){result=ESP_FAIL;saved_errno=errno;}
        else for(;;){
            size_t na=fread(work->read,1,sizeof(work->read),a);
            size_t nb=fread(work->verify,1,sizeof(work->verify),b);
            if(na!=nb || memcmp(work->read,work->verify,na)!=0 || ferror(a)||ferror(b)){
                result=ESP_FAIL;saved_errno=EIO;break;
            }
            if(na<sizeof(work->read))break;
        }
        if(a&&fclose(a)!=0){result=ESP_FAIL;saved_errno=EIO;}
        if(b&&fclose(b)!=0){result=ESP_FAIL;saved_errno=EIO;}
    }
    memset(work,0,sizeof(*work));
    free(work);
    if(result!=ESP_OK){(void)unlink(dst);return sd_error("COPY VERIFY",saved_errno?saved_errno:EIO);}
    return ESP_OK;
}
static esp_err_t copy_directory(const char *source_dir,const char *dest_dir,int depth)
{
    if(depth>MAX_RECURSION)return sd_error_arg("FOLDER NESTING TOO DEEP");
    if(mkdir(dest_dir,0775)!=0)return sd_error("COPY MKDIR",errno);
    DIR *d=opendir(source_dir);
    if(!d)return sd_error("COPY OPEN FOLDER",errno);
    /* Keep pathname storage off the small ESP32-S3 main task stack. */
    struct copy_paths {char src[SD_PATH_SIZE],dst[SD_PATH_SIZE];};
    struct copy_paths *paths=malloc(sizeof(*paths));
    if(!paths){closedir(d);return sd_error("COPY MEMORY",ENOMEM);}
    esp_err_t result=ESP_OK;
    struct dirent *ent;
    errno=0;
    while((ent=readdir(d))!=NULL){
        if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;
        if(!sd_browser_name_valid(ent->d_name)){
            result=sd_error_arg("UNSUPPORTED NAME IN FOLDER");break;
        }
        int a=snprintf(paths->src,sizeof(paths->src),"%s/%s",source_dir,ent->d_name);
        int b=snprintf(paths->dst,sizeof(paths->dst),"%s/%s",dest_dir,ent->d_name);
        if(a<=0||b<=0||(size_t)a>=sizeof(paths->src)||(size_t)b>=sizeof(paths->dst)){
            result=sd_error_arg("COPY PATH TOO LONG");break;
        }
        struct stat st;
        if(stat(paths->src,&st)!=0){result=sd_error("COPY LOOKUP",errno);break;}
        if(S_ISDIR(st.st_mode))result=copy_directory(paths->src,paths->dst,depth+1);
        else if(S_ISREG(st.st_mode))result=copy_file_verified(paths->src,paths->dst);
        else result=sd_error_arg("UNSUPPORTED FILE IN FOLDER");
        if(result!=ESP_OK)break;
        errno=0;
    }
    int e=errno;
    free(paths);
    if(closedir(d)!=0 && result==ESP_OK)result=sd_error("COPY CLOSE",errno);
    if(result==ESP_OK&&e)result=sd_error("COPY READ FOLDER",e);
    return result;
}
static bool begins_dir(const char *child,const char *parent)
{
    size_t n=strlen(parent);
    return strncmp(child,parent,n)==0 && (child[n]=='/'||child[n]=='\0');
}
static bool unique_copy_name(char *out,size_t cap,const char *name,unsigned serial)
{
    if(!serial){int n=snprintf(out,cap,"%s",name);return n>0&&(size_t)n<cap;}
    const char *dot=strrchr(name,'.');
    if(dot==name)dot=NULL;
    size_t stem=dot?(size_t)(dot-name):strlen(name);
    const char *ext=dot?dot:"";
    char suffix[14];snprintf(suffix,sizeof(suffix),"_C%02u",serial);
    size_t limit=SD_BROWSER_NAME_MAX-1-strlen(suffix)-strlen(ext);
    if(stem>limit)stem=limit;
    int n=snprintf(out,cap,"%.*s%s%s",(int)stem,name,suffix,ext);
    return n>0&&(size_t)n<cap;
}
esp_err_t sd_browser_copy(const char *source_dir,const char *name,const char *dest_dir,
                           char *result_name,size_t result_size)
{
    if(result_name&&result_size)result_name[0]=0;
    if(!result_name || result_size<SD_BROWSER_NAME_MAX)return ESP_ERR_INVALID_ARG;
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    struct copy_work {
        char src[SD_PATH_SIZE],destroot[SD_PATH_SIZE],dst[SD_PATH_SIZE];
        char relative[SD_BROWSER_DIR_MAX],target[SD_BROWSER_NAME_MAX];
    };
    struct copy_work *w=calloc(1,sizeof(*w));
    if(!w)return sd_error("COPY MEMORY",ENOMEM);
    esp_err_t result=ESP_FAIL;
    struct stat st,dst_st;
    if(!make_path(w->src,sizeof(w->src),source_dir,name)||
       !make_path(w->destroot,sizeof(w->destroot),dest_dir,NULL)){
        result=sd_error_arg("INVALID COPY PATH");goto done;
    }
    if((result=existing_regular(w->src,&st))!=ESP_OK)goto done;
    if(stat(w->destroot,&dst_st)!=0 || !S_ISDIR(dst_st.st_mode)){
        result=sd_error_arg("DEST FOLDER MISSING");goto done;
    }
    if(S_ISDIR(st.st_mode)){
        if(!subdir(w->relative,sizeof(w->relative),source_dir,name)){
            result=sd_error_arg("SOURCE PATH TOO LONG");goto done;
        }
        if(begins_dir(dest_dir,w->relative)){
            result=sd_error_arg("CANNOT COPY FOLDER INTO ITSELF");goto done;
        }
    }
    for(unsigned i=0;i<100;i++){
        if(!unique_copy_name(w->target,sizeof(w->target),name,i)){
            result=sd_error_arg("NAME TOO LONG");goto done;
        }
        if(!make_path(w->dst,sizeof(w->dst),dest_dir,w->target)){
            result=sd_error_arg("COPY PATH TOO LONG");goto done;
        }
        if(stat(w->dst,&dst_st)==0)continue;
        if(errno!=ENOENT){result=sd_error("COPY TARGET CHECK",errno);goto done;}
        result=S_ISDIR(st.st_mode)?copy_directory(w->src,w->dst,0):copy_file_verified(w->src,w->dst);
        if(result==ESP_OK){
            snprintf(result_name,result_size,"%s",w->target);
            snprintf(last_error,sizeof(last_error),"COPY VERIFIED");
        }
        goto done;
    }
    result=sd_error("NO FREE COPY NAME",EEXIST);
done:
    free(w);
    return result;
}
static esp_err_t delete_tree(const char *path,int depth)
{
    if(depth>MAX_RECURSION)return sd_error_arg("FOLDER NESTING TOO DEEP");
    struct stat st;
    if(stat(path,&st)!=0)return sd_error("DELETE LOOKUP",errno);
    if(S_ISREG(st.st_mode)){
        if(unlink(path)!=0)return sd_error("DELETE FILE",errno);
        return ESP_OK;
    }
    if(!S_ISDIR(st.st_mode))return sd_error_arg("UNSUPPORTED FILE TYPE");
    DIR *d=opendir(path);if(!d)return sd_error("DELETE OPEN FOLDER",errno);
    char *child=malloc(SD_PATH_SIZE);
    if(!child){closedir(d);return sd_error("DELETE MEMORY",ENOMEM);}
    esp_err_t result=ESP_OK;
    struct dirent *ent;
    errno=0;
    while((ent=readdir(d))!=NULL){
        if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;
        if(!sd_browser_name_valid(ent->d_name)){
            result=sd_error_arg("UNSUPPORTED FOLDER ENTRY");break;
        }
        int n=snprintf(child,SD_PATH_SIZE,"%s/%s",path,ent->d_name);
        if(n<=0||(size_t)n>=SD_PATH_SIZE){result=sd_error_arg("DELETE PATH TOO LONG");break;}
        result=delete_tree(child,depth+1);
        if(result!=ESP_OK)break;
        errno=0;
    }
    int e=errno;
    free(child);
    if(closedir(d)!=0 && result==ESP_OK)result=sd_error("DELETE CLOSE",errno);
    if(result==ESP_OK&&e)result=sd_error("DELETE READ",e);
    if(result!=ESP_OK)return result;
    if(rmdir(path)!=0)return sd_error("DELETE FOLDER",errno);
    return ESP_OK;
}
esp_err_t sd_browser_delete(const char *dir,const char *name)
{
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    char *path=malloc(SD_PATH_SIZE);
    if(!path)return sd_error("DELETE MEMORY",ENOMEM);
    if(!make_path(path,SD_PATH_SIZE,dir,name)){
        free(path);return sd_error_arg("INVALID DELETE NAME");
    }
    esp_err_t result=delete_tree(path,0);
    free(path);
    if(result==ESP_OK)snprintf(last_error,sizeof(last_error),"ITEM DELETED");
    return result;
}
static bool text_extension(const char *name)
{
    size_t n=strlen(name);
    return (n>=4 && !strcasecmp(name+n-4,".TXT")) ||
           (n>=4 && !strcasecmp(name+n-4,".LOG")) ||
           (n>=3 && !strcasecmp(name+n-3,".MD")) ||
           (n>=4 && !strcasecmp(name+n-4,".CSV"));
}
esp_err_t sd_browser_text_page(const char *dir,const char *name,size_t first_line,
                           char lines[STORAGE_TEXT_ROWS][STORAGE_TEXT_COLS+1],
                           size_t *total_lines,bool *truncated)
{
    if(!lines||!total_lines||!truncated)return ESP_ERR_INVALID_ARG;
    memset(lines,0,STORAGE_TEXT_ROWS*(STORAGE_TEXT_COLS+1));
    *total_lines=0;*truncated=false;
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    if(!name||!text_extension(name))return sd_error_arg("NOT A TEXT FILE");
    char *path=malloc(SD_PATH_SIZE);
    if(!path)return sd_error("TEXT MEMORY",ENOMEM);
    if(!make_path(path,SD_PATH_SIZE,dir,name)){
        free(path);return sd_error_arg("INVALID TEXT PATH");
    }
    FILE *file=fopen(path,"rb");int saved=errno;
    free(path);
    if(!file)return sd_error("READ TEXT",saved);
    char row[STORAGE_TEXT_COLS+1]={0};size_t col=0,count=0,bytes=0;
    bool failed=false,at_newline=false;
    int ch=0;
    while(bytes<65536 && (ch=fgetc(file))!=EOF){
        bytes++; if(ch=='\r')continue;
        if(ch=='\n'){
            row[col]=0;
            if(count>=first_line&&count<first_line+STORAGE_TEXT_ROWS)
                snprintf(lines[count-first_line],STORAGE_TEXT_COLS+1,"%s",row);
            ++count;col=0;row[0]=0;at_newline=true;continue;
        }
        at_newline=false;
        if(col==STORAGE_TEXT_COLS){
            row[col]=0;
            if(count>=first_line&&count<first_line+STORAGE_TEXT_ROWS)
                snprintf(lines[count-first_line],STORAGE_TEXT_COLS+1,"%s",row);
            ++count;col=0;row[0]=0;
        }
        row[col++]=(ch=='\t')?' ':((ch>=32&&ch<=126)?(char)ch:'?');
    }
    if(ferror(file))failed=true;
    if(!failed && bytes==65536 && fgetc(file)!=EOF)*truncated=true;
    if(fclose(file)!=0)failed=true;
    if(failed)return sd_error("READ TEXT",EIO);
    if(col||(!count&&!at_newline)){
        row[col]=0;
        if(count>=first_line&&count<first_line+STORAGE_TEXT_ROWS)
            snprintf(lines[count-first_line],STORAGE_TEXT_COLS+1,"%s",row);
        ++count;
    }
    *total_lines=count;
    return ESP_OK;
}
esp_err_t sd_browser_read_eeprom(const char *dir,const char *name,uint8_t raw[256])
{
    if(!raw||!name||strlen(name)<5||strcasecmp(name+strlen(name)-4,".BIN"))
        return sd_error_arg("SELECT A 256-BYTE BIN");
    if(!sd_available())return ESP_ERR_INVALID_STATE;
    char *path=malloc(SD_PATH_SIZE);
    if(!path)return sd_error("BIN MEMORY",ENOMEM);
    if(!make_path(path,SD_PATH_SIZE,dir,name)){
        free(path);return sd_error_arg("INVALID BIN PATH");
    }
    FILE *f=fopen(path,"rb");int saved=errno;
    free(path);
    if(!f)return sd_error("READ BIN",saved);
    size_t n=fread(raw,1,256,f);int extra=fgetc(f);int close_result=fclose(f);
    if(n!=256||extra!=EOF||close_result!=0)return sd_error("BIN MUST BE 256 BYTES",EINVAL);
    return ESP_OK;
}

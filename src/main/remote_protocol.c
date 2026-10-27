#include "remote_protocol.h"
#include <string.h>
#include <stdio.h>

uint16_t prom_crc16(const uint8_t *bytes,size_t size,uint16_t crc)
{
    for(size_t i=0;i<size;i++){
        crc^=(uint16_t)bytes[i]<<8;
        for(unsigned b=0;b<8;b++)crc=(uint16_t)((crc<<1)^((crc&0x8000u)?0x1021u:0));
    }
    return crc;
}

size_t prom_encode(uint8_t type,const void *payload,uint16_t length,uint8_t *out,size_t cap)
{
    if(!out||length>PROM_MAX_PAYLOAD||cap<(size_t)length+7||(length&&!payload))return 0;
    out[0]=PROM_MAGIC0;out[1]=PROM_MAGIC1;out[2]=type;
    out[3]=(uint8_t)length;out[4]=(uint8_t)(length>>8);
    if(length)memcpy(out+5,payload,length);
    uint16_t crc=prom_crc16(out+2,(size_t)length+3,0xffffu);
    out[5+length]=(uint8_t)crc;out[6+length]=(uint8_t)(crc>>8);
    return length+7u;
}

bool prom_decode_byte(prom_decoder_t *d,uint8_t c,prom_packet_t *out)
{
    if(!d||!out)return false;
    switch(d->state){
        case 0:if(c==PROM_MAGIC0)d->state=1;break;
        case 1:d->state=(c==PROM_MAGIC1)?2:(c==PROM_MAGIC0?1:0);break;
        case 2:d->type=c;d->crc=prom_crc16(&c,1,0xffffu);d->state=3;break;
        case 3:d->length=c;d->crc=prom_crc16(&c,1,d->crc);d->state=4;break;
        case 4:
            d->length|=(uint16_t)c<<8;
            d->crc=prom_crc16(&c,1,d->crc);
            if(d->length>PROM_MAX_PAYLOAD){d->state=c==PROM_MAGIC0?1:0;break;}
            d->used=0;d->state=d->length?5:6;break;
        case 5:
            d->data[d->used++]=c;d->crc=prom_crc16(&c,1,d->crc);
            if(d->used==d->length)d->state=6;
            break;
        case 6:d->received_crc=c;d->state=7;break;
        case 7:{
            d->received_crc|=(uint16_t)c<<8;
            bool valid=d->received_crc==d->crc;
            if(valid){out->type=d->type;out->length=d->length;
                      if(d->length)memcpy(out->payload,d->data,d->length);}
            d->state=c==PROM_MAGIC0?1:0;
            return valid;
        }
        default:d->state=0;
    }
    return false;
}

bool prom_sd_path(const char *rel,char *full,size_t capacity)
{
    if(!rel||!full||rel[0]!='/')return false;
    const size_t n=strlen(rel);
    if(n> PROM_SD_MAX_RELATIVE || n+sizeof("/sdcard")>capacity)return false;
    if(n>1&&rel[n-1]=='/')return false;
    if(n>1){
        const char *start=rel+1;
        while(*start){
            const char *end=strchr(start,'/');
            const size_t len=end?(size_t)(end-start):strlen(start);
            if(!len||(len==1&&start[0]=='.')||
               (len==2&&start[0]=='.'&&start[1]=='.'))return false;
            for(size_t i=0;i<len;i++){
                unsigned char c=(unsigned char)start[i];
                if(c<32||c==127||c=='\\'||c==':'||c=='*'||c=='?'||
                   c=='"'||c=='<'||c=='>'||c=='|')return false;
            }
            start=end?end+1:start+len;
        }
    }
    return snprintf(full,capacity,"/sdcard%s",rel)>0;
}

/* Stateless, bounded encoder. RGB565 pixels are already in wire byte order. */
size_t prom_rle_encode565(const uint8_t *src,size_t count,uint8_t *out,size_t cap)
{
    if(!src||!out||count==0||count>240)return 0;
    size_t i=0,used=0;
    while(i<count){
        size_t run=1;
        while(run<128 && i+run<count &&
              src[2*i]==src[2*(i+run)] && src[2*i+1]==src[2*(i+run)+1])run++;
        if(run>=3){
            if(cap-used<3)return 0;
            out[used++]=(uint8_t)(0x80u|(run-1));
            out[used++]=src[2*i];out[used++]=src[2*i+1];
            i+=run;
        }else{
            size_t start=i;
            i++;
            while(i<count && i-start<128){
                run=1;
                while(run<3 && i+run<count &&
                      src[2*i]==src[2*(i+run)] && src[2*i+1]==src[2*(i+run)+1])run++;
                if(run>=3)break;
                i++;
            }
            const size_t n=i-start;
            if(cap-used<1+2*n)return 0;
            out[used++]=(uint8_t)(n-1);
            memcpy(out+used,src+2*start,2*n);used+=2*n;
        }
    }
    return used;
}

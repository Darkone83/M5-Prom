/*
 * Offline Xbox EEPROM security-section code adapted from
 * Darkone83/XbDiag src/XbDiag/EepromCrypto.cpp.
 * The reference implementation credits PrometheOS and XboxEepromEditor.
 * This module has NO dependency on the original Xbox kernel.
 *
 * Algorithm: Xbox version-dependent SHA1-HMAC IVs + RC4 for bytes 0x14-0x2F.
 * The raw HDD key (0x1C-0x2B) is never printed or persisted decrypted.
 */
#include "image_crypto.h"
#include <string.h>
#include <stdbool.h>

static uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32U-n)); }
typedef struct { uint32_t h[5]; uint8_t block[64]; uint32_t cursor, bitlen; } sha_t;

static void sha_block(sha_t *s)
{
    static const uint32_t k[4] = {0x5A827999u,0x6ED9EBA1u,0x8F1BBCDCu,0xCA62C1D6u};
    uint32_t w[80];
    for (unsigned i=0;i<16;i++) {
        unsigned j=i*4;
        w[i]=((uint32_t)s->block[j]<<24)|((uint32_t)s->block[j+1]<<16)|
             ((uint32_t)s->block[j+2]<<8)|(uint32_t)s->block[j+3];
    }
    for (unsigned i=16;i<80;i++)w[i]=rotl32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for (unsigned i=0;i<80;i++) {
        uint32_t f;
        if(i<20)f=(b&c)|((~b)&d);
        else if(i<40)f=b^c^d;
        else if(i<60)f=(b&c)|(b&d)|(c&d);
        else f=b^c^d;
        uint32_t tmp=rotl32(a,5)+f+e+w[i]+k[i/20];
        e=d;d=c;c=rotl32(b,30);b=a;a=tmp;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;
    s->cursor=0;
}
static void sha_update(sha_t *s,const uint8_t *data,unsigned len)
{
    for(unsigned i=0;i<len;i++) {
        s->block[s->cursor++]=data[i];s->bitlen+=8;
        if(s->cursor==64)sha_block(s);
    }
}
static void sha_final(sha_t *s,uint8_t out[20])
{
    if(s->cursor>55) {
        s->block[s->cursor++]=0x80;
        while(s->cursor<64)s->block[s->cursor++]=0;
        sha_block(s);
        while(s->cursor<56)s->block[s->cursor++]=0;
    } else {
        s->block[s->cursor++]=0x80;
        while(s->cursor<56)s->block[s->cursor++]=0;
    }
    s->block[56]=s->block[57]=s->block[58]=s->block[59]=0;
    s->block[60]=(uint8_t)(s->bitlen>>24);
    s->block[61]=(uint8_t)(s->bitlen>>16);
    s->block[62]=(uint8_t)(s->bitlen>>8);
    s->block[63]=(uint8_t)s->bitlen;
    sha_block(s);
    for(unsigned i=0;i<20;i++)out[i]=(uint8_t)(s->h[i>>2]>>(8*(3-(i&3))));
}

static const uint32_t xbox_hmac_ivs[4][2][5] = {
 {{0x85F9E51Au,0xE04613D2u,0x6D86A50Cu,0x77C32E3Cu,0x4BD717A4u},
  {0x5D7A9C6Bu,0xE1922BEBu,0xB82CCDBCu,0x3137AB34u,0x486B52B3u}},
 {{0x72127625u,0x336472B9u,0xBE609BEAu,0xF55E226Bu,0x99958DACu},
  {0x76441D41u,0x4DE82659u,0x2E8EF85Eu,0xB256FACAu,0xC4FE2DE8u}},
 {{0x39B06E79u,0xC9BD25E8u,0xDBC6B498u,0x40B4389Du,0x86BBD7EDu},
  {0x9B49BED3u,0x84B430FCu,0x6B8749CDu,0xEBFE5FE5u,0xD96E7393u}},
 {{0x8058763Au,0xF97D4E0Eu,0x865A9762u,0x8A3D920Du,0x08995B2Cu},
  {0x01075307u,0xA2F1E037u,0x1186EEEAu,0x88DA9992u,0x168A5609u}}
};

static void xbox_hmac(int version,const uint8_t *data,unsigned len,uint8_t out[20])
{
    sha_t s={0};uint8_t intermediate[20];
    for(unsigned j=0;j<5;j++)s.h[j]=xbox_hmac_ivs[version][0][j];
    s.bitlen=512;sha_update(&s,data,len);sha_final(&s,intermediate);
    memset(&s,0,sizeof(s));
    for(unsigned j=0;j<5;j++)s.h[j]=xbox_hmac_ivs[version][1][j];
    s.bitlen=512;sha_update(&s,intermediate,20);sha_final(&s,out);
    memset(&s,0,sizeof(s));memset(intermediate,0,sizeof(intermediate));
}
typedef struct {uint8_t table[256];uint8_t x,y;} rc4_t;
static void rc4_init(rc4_t *c,const uint8_t *key,unsigned len)
{
    memset(c,0,sizeof(*c));
    for(unsigned i=0;i<256;i++)c->table[i]=(uint8_t)i;
    unsigned j=0;
    for(unsigned i=0;i<256;i++) {
        j=(j+c->table[i]+key[i%len])&255;
        uint8_t tmp=c->table[i];c->table[i]=c->table[j];c->table[j]=tmp;
    }
}
static void rc4_crypt(rc4_t *c,uint8_t *data,unsigned len)
{
    for(unsigned i=0;i<len;i++) {
        c->x++;c->y+=c->table[c->x];
        uint8_t tmp=c->table[c->x];c->table[c->x]=c->table[c->y];c->table[c->y]=tmp;
        data[i]^=c->table[(uint8_t)(c->table[c->x]+c->table[c->y])];
    }
}

int image_crypto_decrypt(uint8_t buffer[256])
{
    if(!buffer)return -1;
    uint8_t ciphertext[28];memcpy(ciphertext,buffer+0x14,28);
    for(int v=0;v<4;v++) {
        uint8_t key[20],plain[28],verify[20];rc4_t rc4;
        xbox_hmac(v,buffer,20,key);
        memcpy(plain,ciphertext,sizeof(plain));
        rc4_init(&rc4,key,sizeof(key));rc4_crypt(&rc4,plain,sizeof(plain));
        xbox_hmac(v,plain,sizeof(plain),verify);
        bool ok=memcmp(verify,buffer,20)==0;
        memset(&rc4,0,sizeof(rc4));memset(key,0,sizeof(key));memset(verify,0,sizeof(verify));
        if(ok) {
            memcpy(buffer+0x14,plain,sizeof(plain));
            memset(plain,0,sizeof(plain));memset(ciphertext,0,sizeof(ciphertext));
            return v;
        }
        memset(plain,0,sizeof(plain));
    }
    memset(ciphertext,0,sizeof(ciphertext));
    return -1;
}

void image_crypto_encrypt(uint8_t buffer[256],int version)
{
    if(!buffer||version<0||version>=4)return;
    uint8_t key[20];rc4_t rc4;
    memset(buffer,0,20);
    xbox_hmac(version,buffer+0x14,28,buffer);
    xbox_hmac(version,buffer,20,key);
    rc4_init(&rc4,key,sizeof(key));
    // Continuous RC4 keystream: confounder (8 bytes) then remaining 20 bytes.
    rc4_crypt(&rc4,buffer+0x14,8);
    rc4_crypt(&rc4,buffer+0x1C,20);
    memset(key,0,sizeof(key));memset(&rc4,0,sizeof(rc4));
}

/* Xbox ATA USER password derives from an ordinary RFC 2104 HMAC-SHA1.
 * This is deliberately separate from xbox_hmac(), which uses Xbox revision
 * specific SHA1 IVs for EEPROM encryption/authentication.
 */
void image_crypto_hmac_sha1_two(const uint8_t *key,size_t key_len,
                                const uint8_t *part1,size_t len1,
                                const uint8_t *part2,size_t len2,
                                uint8_t digest[20])
{
    if(!digest)return;
    if(!key||(len1&&!part1)||(len2&&!part2)){
        memset(digest,0,20);return;
    }
    static const uint32_t sha1_iv[5]={
        0x67452301u,0xEFCDAB89u,0x98BADCFEu,0x10325476u,0xC3D2E1F0u
    };
    uint8_t key_block[64]={0},pad[64],inner[20];
    sha_t hash={0};
    if(key_len>sizeof(key_block)){
        memcpy(hash.h,sha1_iv,sizeof(hash.h));
        sha_update(&hash,key,(unsigned)key_len);
        sha_final(&hash,key_block);
        key_len=20;
        memset(&hash,0,sizeof(hash));
    }else memcpy(key_block,key,key_len);
    for(size_t i=0;i<sizeof(pad);i++)pad[i]=(uint8_t)(key_block[i]^0x36u);
    memcpy(hash.h,sha1_iv,sizeof(hash.h));
    sha_update(&hash,pad,sizeof(pad));
    sha_update(&hash,part1,(unsigned)len1);
    sha_update(&hash,part2,(unsigned)len2);
    sha_final(&hash,inner);
    memset(&hash,0,sizeof(hash));
    for(size_t i=0;i<sizeof(pad);i++)pad[i]=(uint8_t)(key_block[i]^0x5cu);
    memcpy(hash.h,sha1_iv,sizeof(hash.h));
    sha_update(&hash,pad,sizeof(pad));
    sha_update(&hash,inner,sizeof(inner));
    sha_final(&hash,digest);
    memset(&hash,0,sizeof(hash));
    memset(key_block,0,sizeof(key_block));
    memset(pad,0,sizeof(pad));
    memset(inner,0,sizeof(inner));
}

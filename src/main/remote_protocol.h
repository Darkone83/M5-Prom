#pragma once
/* M5 Prom USB protocol, v1. Pure C helpers kept separate for host tests. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PROM_MAGIC0 0xA5u
#define PROM_MAGIC1 0x5Au
#define PROM_PROTOCOL_VERSION 1u
#define PROM_MAX_PAYLOAD 600u
#define PROM_SD_MAX_RELATIVE 216u

/* Host -> Cardputer */
enum {
    PROM_HELLO=0x01, PROM_PING=0x02, PROM_KEY=0x03,
    PROM_SCREEN_FULL=0x04, PROM_QUIT=0x05, PROM_SCREEN_FAST=0x06,
    PROM_PERF_REQUEST=0x07,
    PROM_FS_LIST=0x20, PROM_FS_GET=0x21,
    PROM_FS_PUT_BEGIN=0x22, PROM_FS_PUT_CHUNK=0x23,
    PROM_FS_PUT_DONE=0x24, PROM_FS_MKDIR=0x25,
    PROM_FS_DELETE=0x26, PROM_FS_RENAME=0x27, PROM_FS_CANCEL=0x28,
    /* Cardputer -> Host */
    PROM_HELLO_ACK=0x81, PROM_PONG=0x82,
    PROM_KEY_STATUS=0x83, PROM_PERF_REPLY=0x84,
    PROM_FRAME_START=0x90, PROM_FRAME_ROW=0x91, PROM_FRAME_END=0x92,
    PROM_FRAME_RLE=0x93,
    PROM_FS_ENTRY=0xA1, PROM_FS_DONE=0xA2, PROM_FS_GET_BEGIN=0xA3,
    PROM_FS_DATA=0xA4, PROM_FS_READY=0xA5, PROM_FS_ACK=0xA6
};

/* HELLO_ACK capability flag: optional RGB565 RLE row streaming. */
#define PROM_CAP_RLE 0x08u
/* Optional diagnostic extension; legacy clients may ignore unknown packets. */
#define PROM_CAP_PERF 0x10u
#define PROM_KEY_SEQ_LEGACY 0xffffu
#define PROM_KEY_QUEUED 0u
#define PROM_KEY_RENDERED 1u
#define PROM_KEY_DROPPED 2u
#define PROM_PERF_WIRE_SIZE 56u

enum { PROM_OK=0, PROM_BUSY=1, PROM_BAD_PATH=2, PROM_NO_SD=3,
       PROM_IO_ERROR=4, PROM_EXISTS=5, PROM_TOO_BIG=6, PROM_NOT_FOUND=7,
       PROM_BAD_STATE=8 };

typedef struct {
    uint8_t type;
    uint16_t length;
    uint8_t payload[PROM_MAX_PAYLOAD];
} prom_packet_t;

typedef struct {
    uint8_t state;
    uint8_t type;
    uint16_t length;
    uint16_t used;
    uint16_t crc;
    uint16_t received_crc;
    uint8_t data[PROM_MAX_PAYLOAD];
} prom_decoder_t;

uint16_t prom_crc16(const uint8_t *bytes,size_t size,uint16_t initial);
/* Returns true when a whole, CRC-verified packet has been decoded. */
bool prom_decode_byte(prom_decoder_t *d,uint8_t byte,prom_packet_t *out);
/* Encodes packet into out; return 0 on invalid length/capacity. */
size_t prom_encode(uint8_t type,const void *payload,uint16_t length,
                   uint8_t *out,size_t capacity);
/* Accept only root-relative paths, reject traversal, ambiguous separators. */
bool prom_sd_path(const char *relative,char *full,size_t capacity);

/* Compress an even-length RGB565 byte row using PackBits-style runs.
 * Literal control 0x00..0x7F: (control+1) pixels follow as raw pairs.
 * Repeat control 0x80..0xFF: ((control&127)+1) copies of next pixel.
 * Returns zero for invalid input or insufficient output buffer.
 * Only use on the wire when the result is smaller than the original row. */
size_t prom_rle_encode565(const uint8_t *pixels,size_t pixel_count,
                          uint8_t *out,size_t capacity);

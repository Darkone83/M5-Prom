"""M5 Prom Remote protocol v1: length-framed, CRC16-protected messages.

This file intentionally has no third-party dependencies, so the wire protocol
can be tested without a Cardputer, Qt, or a serial port.
"""
from __future__ import annotations

import struct
from binascii import crc_hqx
from dataclasses import dataclass

MAGIC = b"\xA5\x5A"
VERSION = 1
MAX_PAYLOAD = 600
WIDTH, HEIGHT = 240, 135
ROW_BYTES = WIDTH * 2

HELLO, PING, KEY, FULL, QUIT, FAST, PERF_REQUEST = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
FS_LIST, FS_GET, FS_PUT_BEGIN, FS_PUT_CHUNK, FS_PUT_DONE = 0x20, 0x21, 0x22, 0x23, 0x24
FS_MKDIR, FS_DELETE, FS_RENAME, FS_CANCEL = 0x25, 0x26, 0x27, 0x28
HELLO_ACK, PONG, KEY_STATUS, PERF_REPLY = 0x81, 0x82, 0x83, 0x84
FRAME_START, FRAME_ROW, FRAME_END, FRAME_RLE = 0x90, 0x91, 0x92, 0x93
CAP_RLE = 0x08
CAP_PERF = 0x10
KEY_QUEUED, KEY_RENDERED, KEY_DROPPED = 0, 1, 2
# version, reserved, FreeRTOS tick_ms, twelve little-endian counters, two uint16s
PERF_FORMAT = "<BBH12IHH"
PERF_SIZE = struct.calcsize(PERF_FORMAT)
FS_ENTRY, FS_DONE, FS_GET_BEGIN, FS_DATA, FS_READY, FS_ACK = 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6
STATUS = {0:"Success",1:"SD busy; retry shortly",2:"Invalid SD path",3:"SD card unavailable",
          4:"SD read/write failed",5:"Destination already exists",6:"File exceeds transfer limit",
          7:"File or folder not found",8:"Operation not ready"}


def crc16(data: bytes, initial: int = 0xFFFF) -> int:
    """CCITT-FALSE. Use CPython's native implementation for fast USB decoding."""
    return crc_hqx(data, initial)


def unpack_rle_row(encoded: bytes) -> bytes:
    """Decode exactly 240 RGB565 pixels; reject truncated/oversized rows."""
    output = bytearray(ROW_BYTES)
    index = 0
    used = 0
    while index < len(encoded):
        ctrl = encoded[index]
        index += 1
        count = (ctrl & 0x7F) + 1
        bytes_needed = count * 2
        if used + bytes_needed > ROW_BYTES:
            raise ValueError("Compressed screen row exceeds display width")
        if ctrl & 0x80:
            if index + 2 > len(encoded):
                raise ValueError("Truncated repeated RGB565 pixel")
            pair = encoded[index:index+2]
            index += 2
            output[used:used+bytes_needed] = pair * count
        else:
            if index + bytes_needed > len(encoded):
                raise ValueError("Truncated literal RGB565 pixels")
            output[used:used+bytes_needed] = encoded[index:index+bytes_needed]
            index += bytes_needed
        used += bytes_needed
    if used != ROW_BYTES:
        raise ValueError("Compressed screen row has incorrect pixel count")
    return bytes(output)


def pack(kind: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("USB command exceeds the protocol packet limit")
    body = struct.pack("<BH", kind, len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


@dataclass(frozen=True)
class Packet:
    kind: int
    payload: bytes


class Decoder:
    """Resynchronizes after USB log output or a damaged packet."""
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[Packet]:
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 7:
            start = self.buffer.find(MAGIC)
            if start < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer[-1:] == MAGIC[:1] else b""
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 7:
                break
            kind, size = struct.unpack_from("<BH", self.buffer, 2)
            if size > MAX_PAYLOAD:
                del self.buffer[:2]
                continue
            total = 7 + size
            if len(self.buffer) < total:
                break
            received = struct.unpack_from("<H", self.buffer, 5 + size)[0]
            expected = crc16(self.buffer[2:5 + size])
            if received == expected:
                result.append(Packet(kind, bytes(self.buffer[5:5 + size])))
                del self.buffer[:total]
            else:
                del self.buffer[:2]
        # Keep memory bounded if connected to the wrong COM device.
        if len(self.buffer) > MAX_PAYLOAD + 7:
            self.buffer[:] = self.buffer[-(MAX_PAYLOAD + 7):]
        return result


def sd_child(folder: str, name: str) -> str:
    """Construct one remote FAT path, rejecting dot/slash/backslash traversal."""
    if not name or name in (".", "..") or any(c in name for c in "/\\\0"):
        raise ValueError("Invalid file or folder name")
    for c in name:
        if ord(c) < 32 or c in ':*?"<>|':
            raise ValueError("Unsupported character in FAT filename")
    result = folder.rstrip("/") + "/" + name
    if len(result.encode("utf-8")) > 216:
        raise ValueError("SD path is too long")
    return result

# M5 Prom USB Remote Protocol v1

The transport uses the ESP32-S3's **USB-OTG vendor bulk interface** (prototype VID:PID `CAFE:4055`), with endpoint OUT `0x01` and IN `0x81`. Windows discovers it via **WinUSB** using a BOS / Microsoft OS 2.0 descriptor. It is not a UART and has no COM port in normal operation. Byte order is little-endian for control fields, except screen pixel data (RGB565 high byte first, matching the ST7789 wire representation).

```
Packet = 0xA5 0x5A + type:u8 + length:u16le + payload[length] + CRC16:u16le
CRC16 = CCITT-FALSE, init 0xFFFF, over type + length + payload
Maximum payload = 600 bytes
```

Host commands: `01 HELLO(version=1)`, `02 PING`, `03 KEY(type,ascii)`, `04 FULL`, `05 QUIT`, optional `06 FAST(enable:u8)`; `20 LIST(path)`, `21 GET(path)`, `22 PUT_BEGIN(size:u32le,path)`, `23 PUT_CHUNK(data)`, `24 PUT_DONE`, `25 MKDIR(path)`, `26 DELETE(path)`, `27 RENAME(old,NUL,new)`, `28 CANCEL`.

Device notifications: `81 HELLO_ACK(version,width:u16,height:u16,flags)`, `82 PONG`; `90 FRAME_START(id:u16)`, `91 FRAME_ROW(id:u16,row:u8,pixels:480 bytes)`, `92 FRAME_END(id:u16)`, optional `93 FRAME_RLE(id:u16,row:u8,PackBits-RGB565)`; `A1 ENTRY(is_dir:u8,size:u32,name)`, `A2 FS_DONE(code:u8,message:NUL)`, `A3 GET_BEGIN(size:u32)`, `A4 DATA(bytes)`, `A5 READY(0)`, `A6 ACK(received:u32)`.

The host sends PING once per second; the device leaves PC Mode after approximately 6.5 seconds without a valid host packet. Rows not changed since the last frame are omitted. A complete frame is sent after the first handshake and upon FULL request. File uploads use chunk-by-chunk acknowledgements to avoid exhausting the Cardputer's USB receive buffers.

Filesystem paths start with `/` and resolve **beneath `/sdcard` only**. Parent traversal, ambiguous path separators and device filesystem paths are rejected. The shared filesystem operation gate prevents concurrent EEPROM maintenance and remote file mutation. Transfers use temporary files and byte-limited chunks; the PC never mounts raw microSD sectors.

**Design note:** TinyUSB runs USB-OTG and owns the internal USB PHY.
The Serial/JTAG device is not active during M5 Prom firmware execution.
USB packet framing and CRC16 are unchanged; the Windows companion uses libusb
bulk transfers through WinUSB. ROM BOOT mode still provides a flashing recovery
path without eFuse modifications. Native USB and the earlier serial transport
are not simultaneously available on the same physical USB connection.

## Fast screen extension (compatible with protocol v1)

`HELLO_ACK.flags & 0x08` advertises RLE-capable firmware. A new desktop
client sends `FAST(1)` **only if that capability bit is present**. The device
keeps sending the original raw `FRAME_ROW` packets until FAST is requested.
This means an older desktop companion still works with the optimized firmware,
and the new companion still works with the original remote-enabled firmware.

`FRAME_RLE` uses **one complete 240-pixel row** per packet. The first three
bytes remain `frame_id:u16le,row:u8`. The remainder uses pixel-pair runs:

- Control byte `0x00..0x7F`: copy `control + 1` literal RGB565 pixel pairs.
- Control byte `0x80..0xFF`: repeat the following RGB565 pixel pair
  `(control & 0x7F) + 1` times.
- RGB565 bytes are transmitted in the same **big-endian pixel order** as raw
  rows. Decoded rows must be exactly **480 bytes / 240 pixels**; malformed
  compressed rows are rejected, and the host requests a fresh screen.
- The sender chooses RLE only when it produces fewer bytes than the 480-byte
  raw row. Photographic or noisy rows automatically use `FRAME_ROW` instead.

The firmware sends up to eight changed rows per scheduling cycle (previously
two). Consecutive unchanged frames no longer trigger a host redraw. The
physical Cardputer shows PC MODE while the PC displays live frames; the
unchanged physical PC MODE card is not retransferred over SPI after each PC
keyboard operation. USB packet checksums and original SD transfer formats are
unchanged.

## Pass 1 optional diagnostics (backward-compatible)

`HELLO_ACK.flags & 0x10` indicates the device implements the optional Pass 1
telemetry extension. The base protocol version stays 1. Without this bit, the
host sends the original two-byte `KEY(type,ascii)` and does not request stats.

- `07 PERF_REQUEST`: zero-length host request, normally once a second.
- `84 PERF_REPLY`: 56-byte payload packed as little-endian `<BBH12IHH>`:
  `version=1`, reserved byte, FreeRTOS tick length in ms, followed by twelve
  `uint32`: uptime_ms, USB_RX_bytes, completed_USB_TX_bytes, frames_queued,
  keys_queued, keys_rendered, keys_dropped, screen_TX_backpressure_events,
  missed_framebuffer_publish_locks, diagnostic_ACK_drops, free_internal_heap_bytes,
  minimum_free_internal_heap_bytes. Last two `uint16`: peak_key_queue_depth,
  remote_task_stack_watermark_bytes. Counters wrap modulo 2³².
- `03 KEY(type,ascii,sequence:u16le)` is accepted in addition to the original
  two-byte form. `sequence=65535` is reserved for legacy untracked keys.
- `83 KEY_STATUS(sequence:u16le,status:u8,device_delay_ms:u16le)`:
  `status=0` accepted into key queue, `status=1` returned from UI handler/render,
  `status=2` discarded because the bounded queue was full. Delays saturate at
  65535 ms. Status packets may be delayed by USB IN backlog; they are
  measurement aids, not timing guarantees or flow-control signals.

An ACK or a queued frame is **not equivalent** to a frame physically displayed
by Windows; the remote app measures the latter separately in a subsequent pass.
This extension does not modify EEPROM read/write, SD operations or the original
RGB565 compression protocol.

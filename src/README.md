# Cardputer Xbox EEPROM Manager — v0.2 source prototype

**Hardware target:** M5Stack Cardputer V1.1, K132-V11, Stamp-S3A, ESP32-S3FN8, 8 MB flash, **no PSRAM**. Not the Cardputer ADV.

**Firmware framework:** Native Espressif **ESP-IDF 5.5.x** (VS Code / Espressif extension); no Arduino or custom bootloader needed.

> Development-source prototype. Offline image/crypto unit tests have been run on a host computer, **but an ESP-IDF build and Cardputer hardware testing have not been performed in this environment**. Physical EEPROM writes are intentionally **not implemented**. No claims are made that the LCD, keyboard, SD card or Xbox EEPROM interface have been validated on hardware.

## Intended workflow

1. Mount microSD without formatting it.
2. Select **READ EEPROM**. Read the chip twice using 7-bit I²C address `0x54` over Grove GPIO1=SCL / GPIO2=SDA at 100 kHz. Abort if either read fails or the images differ.
3. Immediately create the serial-number directory and write a **new** `ORIGINAL_####.BIN` file (256 raw encrypted bytes). Reopen the file and verify every byte. Do **not** open an editable session until this backup succeeds.
4. Copy the raw EEPROM into an independent in-RAM **working buffer**. All subsequent inspection, repair and editing take place locally, not on the console or the SD original file.
5. **SAVE TO SD** stores a **new** `EDIT_####.BIN` (if changed) or `SNAPSHOT_####.BIN` (otherwise). Original files are never intentionally overwritten.
6. **LOAD FROM SD** browses serial directories and `.BIN` files to work on an existing image without the Xbox connected.
7. **WRITE TO XBOX** is visibly locked and its functionality does not exist in v0.2. When the LPC power adapter and protocol behavior have been measured and verified, introduce a separate guarded write/verify flow.

## SD layout

`/sdcard` is the firmware mount point and corresponds to the SD card root on a PC.

```text
/EEPROMs/
  123456789012/
    ORIGINAL_0001.BIN
    ORIGINAL_0002.BIN
    EDIT_0001.BIN
    EDIT_0002.BIN
    SNAPSHOT_0001.BIN
  UNKNOWN_A936E51F/
    ORIGINAL_0001.BIN
```

`<SN>` is the 12-digit console serial at EEPROM offset `0x34` (validated for printable decimal characters, not all identical). If invalid, use `UNKNOWN_<CRC32>` as a *directory identifier only* (CRC32 is a file fingerprint, **not** an Xbox checksum).

Directories are derived from the **source EEPROM** before editing. Editing the serial number **does not rename or move** the working session to a different directory: the original identity is retained for auditability. Backup filenames are created with exclusive-create semantics, and a saved file is reopened and byte-verified. The file browser currently lists the first 40 directories/files in alphabetical order; pagination beyond 40 entries is a future task.

## Operations menu

1. **Read EEPROM** — two matching on-chip reads, immediate verified original backup, then working copy.
2. **Save to SD** — new versioned 256-byte encrypted raw image.
3. **Load from SD** — select a serial folder and an existing 256-byte `.BIN` file.
4. **Inspect EEPROM** — serial, MAC prefix, checksum status, security/HMAC status, video, game region, language, DVD region.
5. **Edit EEPROM** — local video standard, game region (only if security section authenticates), language, DVD region, video modes, AC3/DTS, serial and MAC address.
6. **Repair EEPROM** — local, individually selected checksum rebuilds and field fixes for bad video standard, language, DVD region or unsupported audio flags.
7. **Write to Xbox** — **disabled** until adapter bench verification and write implementation.
8. **Revert working copy** — restore RAM buffer from its original loaded bytes; SD files stay unchanged.
9. **Pinout / Hardware** — quick reference.

The UI is intentionally designed around the **240×135 screen** using a scrolling five-row list, a readable 5×7 bitmap font, a fixed header/status area and a footer help bar.

### Keyboard

- **W/S** — select rows, or **Fn+; / Fn+.** to move up/down.
- **A/D** — previous/next selectable value in the editor; **Fn+, / Fn+/** also work.
- **Enter** — select or commit a local change.
- **Backspace** — go back, or erase a character in text editing.
- **Fn+`** — cancel text editing or dialogs.
- **Serial/MAC editor** uses the physical number/letter keys, with 12-character validation (MAC entry is 12 hexadecimal digits with no delimiters).

## Local image logic adapted from XbDiag

Reused **algorithms and field information** from `Darkone83/XbDiag`:

- `EepromRepair.cpp`: DWORD-with-carry checksum algorithm, factory checksum `0x30` covering `0x34..0x5F`, user checksum `0x60` covering `0x64..0xBF`.
- `EepromSettings.cpp`: checksum rebuilding, video/audio/region settings, validation rules.
- `EepromView.cpp`: field offsets and types.
- `EepromCrypto.cpp`: version-dependent two-pass HMAC-SHA1 and RC4 of the security block (`0x14..0x2F`).

The security routines are ported to standalone C and run **only on an in-memory copy** for checking the IV/authentication or rebuilding the region-encrypted portion. The HDD key is never displayed or logged. **No Xbox kernel functions** (`ExSaveNonVolatileSetting`, `ExQueryNonVolatileSetting`, `HalWriteSMBusValue`) are used anywhere in this project.

### Repair limits and data integrity

- **Repair does not run automatically.** Each fix is chosen separately, and a confirmation is required.
- Rebuilding a checksum proves internal arithmetic consistency; it does **not** prove that its payload is semantically correct, and it cannot restore lost serials, MACs or HDD keys.
- Failed HMAC validation is a **detect-only** condition, never automatically re-encrypted. Game-region editing is denied unless the encrypted section can be authenticated first.
- Values set by the repair menu (e.g. NTSC-M, English, region-free DVD) are defaults, *not* recovered historical values. Apply them only when intentionally appropriate.
- The Cardputer never writes a repaired image to the physical EEPROM in v0.2.
- The SD `.BIN` files include console identity and security-sensitive data; retain backups privately.

## Build and flash

1. Open `cardputer_xbox_eeprom_v0.2` in VS Code with the Espressif ESP-IDF extension set up for **ESP-IDF 5.5.x**.
2. Use `esp32s3` target and an available COM port.
3. From an ESP-IDF terminal:

   ```powershell
   idf.py set-target esp32s3
   idf.py build
   idf.py -p COMx flash monitor
   ```

4. To enter ROM download mode: turn the Cardputer off, hold G0, power on and release G0. The tool uses the standard ESP-IDF second-stage bootloader, not an M5-specific bootloader.

## Host-side offline unit tests

No ESP-IDF installation is required for the pure image-engine test:

```bash
cd cardputer_xbox_eeprom_v0.2
cc -std=c11 -Wall -Wextra -Werror -pedantic -I main \
  tests/test_image.c main/image.c main/image_crypto.c -o test_image
./test_image
```

Tests exercise all four security IVs using generated test data, checksum repair, offline editing of the region/serial/MAC and RAM reversion. **These are synthetic tests, not known-good hardware dump compatibility tests.** Real cross-verification with XbDiag/PicoPromSD output is still needed before enabling physical writing.

## GPIO pin map and adapter limitations

| Cardputer V1.1 Grove | Xbox LPC header | Description |
|---|---|---|
| GPIO1 / white | Pin 13 | SMBus SCL |
| GPIO2 / yellow | Pin 14 | SMBus SDA |
| GND / black | Pin 2 | Common GND |
| 5V / red | **Adapter power circuit only** | Do not connect to EEPROM supply directly |

The Xbox EEPROM and SMBus signals are 3.3V logic; the ESP32-S3 pins are **not** 5V-tolerant. A console unplugged from AC is different from a console merely in standby. Injecting 3.3V into LPC pin 15 could energize a much larger motherboard rail than the EEPROM alone, and the V1.1's Grove connector offers no extra GPIO dedicated to target-voltage/current measurement or power-enable control. Verify supply requirements, backfeeding, line pullups, bus isolation and current limit on the actual board revision before connecting the Cardputer. This is not an adapter schematic or a validated target-power solution.

## References

- M5Stack V1.1: https://docs.m5stack.com/en/core/Cardputer_v1.1
- M5Cardputer keyboard matrix: https://github.com/m5stack/M5Cardputer/tree/master/src/utility/Keyboard/KeyboardReader
- XbDiag EEPROM implementations: https://github.com/Darkone83/XbDiag/tree/main/src/XbDiag
- PicoPromSD reference: https://github.com/dtomcat/picoPromSD
- ESP-IDF FATFS API: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/storage/fatfs.html

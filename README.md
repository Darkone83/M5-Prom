<div align="center">
  <img src="img/logo.png" alt="M5 Prom" width="400">
  <img src="img/darkone83.png" alt="Darkone83 Customs" width="400">
</div>

# M5 Prom

**A portable, standalone original Xbox EEPROM reader, editor and programmer for the M5Stack Cardputer.**

M5 Prom is designed for console repair and recovery work. It uses a purpose-built Cardputer-to-LPC adapter to access the original Xbox's 256-byte EEPROM **while the console is powered off**, and stores backups and edited images as ordinary files on a FAT32 microSD card. No running Xbox dashboard, computer connection or network is required for EEPROM operations.

> **Release status:** The current source baseline is **v0.9.3 (pre-release)**. Some hardware validation is still in progress. The [`firmware/`](firmware/) directory may be empty until compiled release images are available. You can build the firmware from [`src/`](src/) in the meantime.

## Features

- Read the Xbox EEPROM twice, compare the results and save a verified original backup before editing.
- Inspect console identity, security/checksum status and supported configuration fields.
- Edit and repair an **in-memory working copy** without changing the physical EEPROM until Write is explicitly selected.
- Create a new retail donor EEPROM from scratch or clone an existing image from RAM or microSD.
- Save and load versioned **256-byte `.BIN`** files without overwriting previous backups.
- Program a connected EEPROM with prewrite backup, explicit confirmation, progress reporting and full readback verification.
- View SD filesystem and EEPROM-detection diagnostics, battery information and adapter pinout.
- Configure brightness, automatic dimming, sounds and Wi-Fi; optionally manage SD files through a local web browser.

## Supported hardware

- **M5Stack Cardputer V1.1** (Stamp-S3A / ESP32-S3, 8 MB flash). The current firmware pin mapping is **not for Cardputer ADV**.
- **Original Xbox** LPC connection using the M5 Prom adapter PCB, a correctly oriented cable and a compatible EEPROM.
- FAT32 microSD card for backup, restore and donor files.
- USB data cable for firmware flashing and serial diagnostics.

**Important:** M5 Prom is an **offline-only programmer**. Power the Xbox **off before connecting or operating the adapter**, and do not use it with a powered-on console. For an offline programming session, use the Cardputer's **5V IN/OUT switch in the OUT position** to power the adapter as designed. Do not confuse that switch with the Cardputer's main power control. Follow the PCB markings and the schematic in [`img/`](img/) for connector orientation; never connect an unregulated 5V line directly to the EEPROM's 3.3V supply.

### Adapter connection reference

The adapter's **four-pin Cardputer/Grove connector (CN1)** carries `SCL`, `SDA`, `5V` and `GND` (pins **1–4**, respectively). On the original Xbox LPC side, the design routes **SCL to LPC 13**, **SDA to LPC 14**, **ground to LPC 2/12**, and **regulated 3.3V to LPC 9/15**. Refer to the actual PCB silkscreen and schematic in [`img/`](img/) before connecting a cable; **connector orientation matters**.

The fabrication package (Gerbers, BOM and pick-and-place files) is maintained in [`pcb/`](pcb/). These are manufacturing files, not substitutes for checking the assembled board and cable before use.

## Getting started

1. Insert a FAT32 microSD card into the Cardputer. Existing files can remain on the card; M5 Prom **does not format it**.
2. With the Xbox powered **off**, connect the adapter to the Xbox LPC header and the Cardputer Grove connector, checking orientation before applying adapter power.
3. Set the Cardputer 5V direction switch to **OUT**, then start M5 Prom.
4. From the home screen press **`0`** for Settings, select **Diagnostics** and press **Enter**. Confirm that the SD test shows **RW VERIFIED** and that the EEPROM is detected at address **`0x54`**.
5. Return to the home screen. Select **Read** and press **Enter**. M5 Prom performs two EEPROM reads, checks that they match and saves an **`ORIGINAL_####.BIN`** backup to microSD.
6. Inspect, edit or save the working image as needed. Use **Write** only when you are ready to program the physical chip.

If an SD test fails, fix it before attempting to write an EEPROM. A verified prewrite SD backup is mandatory for physical programming.

## Home screen

M5 Prom uses a **3 × 3 menu**. Press **`1`–`9`** to highlight a command, then **Enter** to open or run it; you can also navigate with the directional keys.

| Key | Command | What it does |
|:---:|---|---|
| `1` | **Read** | Read the physical EEPROM twice, verify matching data and save an original backup. |
| `2` | **Save** | Save the current working EEPROM image as a new `.BIN` file on microSD. |
| `3` | **Load** | Browse stored EEPROM images on microSD and load one into RAM. |
| `4` | **Inspect** | View serial/MAC and EEPROM validation details. |
| `5` | **Edit** | Change supported identity, region and configuration fields in RAM. |
| `6` | **Repair** | Inspect and apply supported field/checksum fixes to the working copy. |
| `7` | **Write** | Prepare a verified backup, confirm and program the attached EEPROM. |
| `8` | **Revert** | Discard edits and restore the current image's original in-memory contents. |
| `9` | **Donor** | Create a new EEPROM image or clone an existing one. |

### Keyboard reference

| Keys | Action |
|---|---|
| `W` / `S` | Move up / down in menus and lists. |
| `A` / `D` | Move left / right on the home grid; adjust selectable values. |
| Arrow keys | Directional navigation where supported by the keyboard. |
| `Enter` | Select, open, confirm or apply. |
| `Backspace` | Go back; when typing, delete the previous character. |
| `Esc` | Cancel an active text-entry screen. |
| `0` (home) | Open **Settings**. |
| `B` (home) | Show **Battery** information. |
| `P` (home) | Show adapter **Pinout**. |
| `M` (home) | Toggle UI sound effects. |
| `Tab` (Editor) | Change editor field group. Keys `1`–`6` select a group directly. |
| `Tab` (Wi-Fi password) | Show or hide the typed password. |
| `W` then `Enter` (Write confirmation) | Arm and begin the confirmed physical EEPROM write. |

In **Edit**, use `W`/`S` to select a field and `A`/`D` or `Enter` to change it; text fields open a dedicated editor. Edits remain in RAM until you use **Save** or **Write**. Backing out of a screen is **not** a physical EEPROM write.

### Creating a donor EEPROM

Open **Donor** (`9`) and choose one of the following:

- **From Scratch:** Choose the Xbox EEPROM revision family. M5 Prom generates a new donor without needing a source image and automatically saves it to microSD as `DONOR_####.BIN`. If saving fails, the generated image remains **unsaved in RAM** until you save it manually.
- **Copy Loaded Image:** Clone the current RAM image. Edit as needed, then select **Save** to create a new file.
- **Copy Image From SD:** Browse an existing `.BIN` image, load a copy into RAM, edit it and select **Save**. The source file is not overwritten.

A scratch donor generates a **new HDD key**. It will **not unlock a drive locked using the original EEPROM key**; retain your original backup before replacing an EEPROM.

### Writing an EEPROM

1. Load, create or edit the intended image, then verify its details.
2. Select **Write** (`7`, then Enter). M5 Prom reads the target, compares two physical reads, and saves verified **`PREWRITE_####.BIN`** and **`TO_WRITE_####.BIN`** files to SD.
3. Review the target/source details and any security or revision warning.
4. Press **`W`** to arm, then **Enter** to begin programming. **Backspace** cancels while still on the confirmation page.
5. Watch the progress indicator. Programming progress tracks EEPROM write acknowledgements, then completes **two full verification reads**. **100% means the entire EEPROM was verified.**

**Never disconnect or power down during a write.** Once physical programming begins there is **no mid-write cancellation**. If interrupted or reported unsuccessful, the EEPROM may be partially programmed; retain the prewrite backup for recovery.

## microSD and files

The card uses its **existing FAT32 filesystem**, mounted internally at `/sdcard`. EEPROM backup and donor files are stored as normal files (not raw SD sectors):

```text
/EEPROMs/
└── <12-digit-serial>/
    ├── ORIGINAL_0001.BIN
    ├── DONOR_0001.BIN
    ├── PREWRITE_0001.BIN
    └── TO_WRITE_0001.BIN
```

File names and sequence numbers vary by operation. M5 Prom also supports a fallback folder name when the serial number is invalid. Each EEPROM image is **exactly 256 bytes**, with a new version created instead of overwriting an existing file. Treat backups as sensitive: they contain console-specific identity and security material.

If saves fail, open **Settings → Diagnostics → Enter** for a non-destructive filesystem read/write test. You can also use **Settings → Remount SD** with **Web Files OFF**. FAT long-filename support is required when building from source; **do not format the SD card just because a save failed**.

## Settings and wireless file manager

Press **`0`** on the home screen to access:

- **Brightness:** 10–100%; **Auto Dim:** Off, 30 s, 1 min, 2 min or 5 min.
- **Sound Effects:** On/Off.
- **Wi-Fi Mode:** Off, Auto, Station or SoftAP. Auto attempts saved Station Wi-Fi first and falls back to SoftAP if unavailable.
- **Join Network:** Scan nearby **2.4 GHz** Wi-Fi networks and enter credentials.
- **Web Files:** On/Off (**off by default**).
- **Network Info:** View the current IP address and SoftAP details.
- **Remount SD** and **Diagnostics:** Check removable storage and EEPROM communication.

The device's SoftAP network is named **`M5Prom-XXXX`** (device-specific suffix) with default password **`myprom83`**. To use the file manager, enable **Web Files**, connect a phone or computer to the same local network, and open the IP address shown in **Network Info** in a browser. The web manager supports browsing, upload/download, directory creation, renaming and deletion on the SD card.

The browser interface is **intended only for a trusted private network**. It uses HTTP without a separate web login. Do not port-forward it or expose it to the internet, and switch **Web Files OFF** when not needed. During protected EEPROM/SD operations, conflicting web file requests are temporarily rejected rather than allowed to interrupt a backup.

## Flashing M5 Prom (GUI)

The included **PySide6 flasher** is designed for technicians and end users. You do **not** need ESP-IDF, a command line, or a particular folder structure to install compiled firmware.

1. Connect the Cardputer to the PC with a USB-C **data** cable, with the Xbox adapter disconnected.
2. On Windows, double-click **`M5Prom-Flasher.bat`** in the repository root. The first launch automatically sets up a private Python environment and downloads the needed GUI and flashing packages. Internet is required only for initial setup.
3. Select the Cardputer's **USB port** (or click **Refresh**).
4. Use the three **Browse** buttons to select the correct `.bin` files, wherever they are saved:
   - **Bootloader** — `bootloader.bin`
   - **Partitions** — `partitions.bin` or `partition-table.bin`
   - **Firmware** — the compiled M5 Prom application, typically `cardputer_xbox_eeprom.bin`
5. Click **Flash**. Follow the progress and wait for the **Success** message before unplugging the Cardputer.

The names above are examples, **not required filenames**. Each image can be selected from a different directory. There is no folder scanning, copying, renaming, or configuration file to prepare. **Use all three binaries from the same compiled release.**

**Initial setup:** The Windows launcher needs **Python 3.10+** on the PC. It will open the official Python download page if Python isn't present; Python packages such as PySide6 and esptool are installed automatically. Subsequent launches work offline. A future self-contained `.exe` could remove even the initial Python requirement. macOS/Linux users with Python 3.10+ can start the same GUI using `python3 tools/flash.py`.

### Connection help

If the port is missing or flashing cannot connect, close any serial monitor and try a different USB data cable. To enter download mode on the Cardputer, with the Xbox adapter disconnected, set its power switch **OFF**, hold **G0**, connect USB-C to the PC, then release **G0** and press **Refresh**. The app also includes a **Connection help** link.

The flasher writes the bootloader at `0x00000`, partition table at `0x08000`, and application at `0x10000` (ESP32-S3 factory layout). It validates each selected file, checks that the application fits the selected partition table, and uses esptool for verified flashing. It **does not mass-erase the ESP32-S3** and it does not modify the microSD card. If the firmware release isn't yet compiled, the `/firmware` directory may be empty.

## Build from source

M5 Prom is an **ESP-IDF C project** targeting the ESP32-S3, not an Arduino sketch. The source project lives in [`src/`](src/), which contains the root `CMakeLists.txt`, `main/`, `sdkconfig.defaults`, and `partitions.csv`.

**Requirements:** ESP-IDF **5.5.x** (developed against **5.5.5**), Python environment supplied by ESP-IDF, CMake/Ninja, and an ESP32-S3 toolchain. In an ESP-IDF-enabled terminal:

```powershell
cd src
idf.py set-target esp32s3
idf.py menuconfig
idf.py fullclean
idf.py build
idf.py -p COM7 flash monitor
```

In `menuconfig`, ensure **Component config → FAT Filesystem support → Long filename support** is enabled (prefer **Long filename buffer in heap**). This is needed for serial-number directories and filenames such as `ORIGINAL_0001.BIN`. `sdkconfig.defaults` supplies intended settings for a fresh build, but an existing `sdkconfig` can override them.

When preparing a precompiled firmware release, copy these three outputs from `src/build/` into `/firmware`:

| Build output | Release file |
|---|---|
| `src/build/bootloader/bootloader.bin` | `firmware/bootloader.bin` |
| `src/build/partition_table/partition-table.bin` | `firmware/partitions.bin` |
| `src/build/cardputer_xbox_eeprom.bin` | `firmware/cardputer_xbox_eeprom.bin` |

Only publish these as a matched set from a successful build. If you have changed the default partition layout, update the flash tool before publishing your binaries.

## Repository structure

```text
M5-Prom/
├── README.md                 # This guide
├── M5Prom-Flasher.bat        # Double-click to start Windows GUI flasher
├── img/                      # logo.png, darkone83.png, PCB renders, schematic
├── pcb/                      # Gerbers, BOM, pick-and-place manufacturing data
├── src/                      # Complete ESP-IDF application project
│   ├── CMakeLists.txt
│   ├── main/
│   ├── partitions.csv
│   └── sdkconfig.defaults
├── firmware/                 # Compiled releases when available
└── tools/
    ├── flash.py             # First-run dependency installer and GUI launcher
    ├── flasher_gui.py       # PySide6 graphical flasher
    ├── flasher_core.py      # Validation for three user-selected files and flash commands
    └── test_flasher.py      # Host-side tests
```

## Troubleshooting

| Symptom | What to check |
|---|---|
| SD card not detected or save fails | FAT32 card, insertion, **Settings → Diagnostics**; check FatFs long filenames in an existing build. Do not reformat a card containing backups. |
| EEPROM not detected | Console powered off, adapter orientation/connections and **Diagnostics** (`0x54`). |
| Read fails | Stable adapter connection; the two physical reads must match. A failed read does not create a new original image. |
| Write refuses to start | A valid working image and a writable microSD are required, along with successful **PREWRITE** and **TO_WRITE** backups. |
| Wi-Fi browser unavailable | Check Wi-Fi mode and IP under **Network Info**, enable **Web Files**, and use a private local connection. |
| GUI flasher reports missing files | Click Browse beside each of the three file types and choose the correct `.bin` from any folder. |
| USB flash connection fails | Choose the correct port, close other serial monitors, try a data-capable cable or enter Cardputer Download Mode (hold G0 while connecting USB). Choose Compatibility speed if necessary. |

## Credits and licensing

M5 Prom is a **Darkone83 Customs** project. The implementation draws on original Xbox EEPROM formats and community research, including the [XboxEepromEditor](https://github.com/Ernegien/XboxEepromEditor) project and Darkone83's XbDiag work.

The source includes EEPROM timezone data derived from **XboxEepromEditor (GPL v2)**; see [`src/third_party_licenses/XboxEepromEditor_GPL-2.0.txt`](src/third_party_licenses/XboxEepromEditor_GPL-2.0.txt) and preserve applicable notices and source-distribution obligations. A separate top-level project license should be specified before the repository is formally released.

M5 Prom is an independent repair tool and is not affiliated with Microsoft or M5Stack.

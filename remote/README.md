# M5 Prom Remote

A single-window USB companion for **M5 Prom v0.9.4** running on the M5Stack Cardputer v1.1.

**No Wi-Fi, separate USB/UART adapter, or extra hardware is required.** Use the Cardputer's USB-C data cable. The PC app connects through the native USB-OTG vendor interface (WinUSB), not a serial COM port.

## Getting started on Windows

1. Install **Python 3.10 or newer**, if it isn't already installed.
2. Double-click **`remote/M5Prom-Remote.bat`**.
3. On first use, the launcher creates a private `.venv` folder inside `remote/` and installs any missing dependencies. It uses **unpinned package names** so pip selects the latest releases compatible with your Python. Internet access is required the first time.
4. Plug the Cardputer into the PC using a **data-capable** USB-C cable. Open M5 Prom firmware with **PC Mode support** (the accompanying patched source must be flashed first).
5. Select **M5 Prom Native USB** from the detected device list and click **Connect**.

With the matching optimized firmware, the remote viewer automatically enables
**Fast Screen** mode: unchanged rows are skipped and changed rows are compressed
when possible. There is no new setting or button. If the Cardputer has the
original USB Remote firmware, the newer PC app automatically uses the original
uncompressed format instead; no firmware downgrade is necessary.

The Cardputer LCD changes to **PC MODE ACTIVE**. The desktop app displays the actual 240×135 M5 Prom screen, enlarged with crisp pixels. Closing or disconnecting the app automatically restores the physical screen after a short heartbeat timeout.

### Controls

- **Live screen:** click inside the display, then use arrow keys, Enter, Backspace, Esc, Tab, or M5 Prom's ordinary single-letter shortcuts. You can type into its existing text editors.
- **Save Screenshot:** save the currently mirrored screen to a PNG file.
- **Start / Stop Recording:** record the screen at 12 fps into a crisp, nearest-neighbor 960×540 MP4 on your PC. This records screen pixels (not audio) and finalizes the file when you stop.
- **microSD File Manager:** browse folders, upload files (up to 16 MB each), download files, create folders, rename and delete. To protect data, uploads refuse to overwrite an existing file, and deleting a non-empty folder is blocked. If a physical EEPROM operation or web transfer owns the SD lock, the app reports **SD busy** rather than forcing access.

Files live on the **existing FAT32 microSD**, not on a second storage volume. USB Mass Storage is **not** enabled, so Windows cannot mount and corrupt the device's FAT32 card while M5 Prom is using it.

### Recordings and screenshots

The app prompts you to choose their save locations on the PC. You do not need to put them in the repository. To avoid accidentally disclosing console identity or HDD keys, review the screen before sharing a video or screenshot.

## Dependencies

The Windows launcher installs any missing packages into `remote/.venv/`:

- PySide6-Essentials — desktop user interface
- pyusb and libusb-package — native USB bulk connection (WinUSB on Windows)
- numpy — pixel conversion
- imageio and imageio-ffmpeg — MP4 recording

**No fixed version numbers or `--only-binary` restrictions.** Already-installed dependencies in the private environment are not needlessly reinstalled. If you deliberately want to upgrade them, delete the `remote/.venv/` directory and restart the launcher.

Manual startup on other platforms is possible with Python 3.10+ and the same dependencies: `python bootstrap.py` from inside `/remote`.

## Firmware integration

Copy the `src/main/` files from the accompanying patch into the current M5 Prom v0.9.4 ESP-IDF source, preserving the `/src/main` layout, and rebuild using **ESP-IDF 5.5.x**. The patch adds:

- `remote_protocol.c/.h`: versioned CRC16-framed packets and SD-path validation.
- `remote_usb.c/.h` plus `remote_native_usb.c/.h`: native USB-OTG vendor bulk transport, PC handshake and heartbeat, dirty-row RGB565 streaming, keyboard event queue and guarded file I/O.
- Small integrations in `main.c`, `display.c` and `main/CMakeLists.txt`.

The optimized display transport uses eight changed-row slots per scheduler
cycle instead of two, and reads USB bulk transfers directly on Windows.
All transfer acknowledgements, HDD recovery operations and EEPROM write
safeguards are unchanged. Live frame rate depends on the USB connection and
Cardputer activity; the recorded video remains fixed at **12 fps**.

The device never enters PC Mode merely because USB is plugged in. It waits for a matching application handshake. Physical EEPROM write validation, confirmation and backup rules remain in force, including during PC control.

## Troubleshooting

- **No M5 Prom USB device:** verify the updated native-USB firmware is flashed, reconnect the USB data cable, and check Device Manager. Windows 10/11 should auto-bind WinUSB using the included descriptors; allow device setup to finish. The old Serial/JTAG COM port will disappear while the app is running.
- **Connected but no picture:** install the supplied firmware additions. Old M5 Prom firmware does not speak the remote protocol. The app requires protocol v1.
- **SD unavailable/busy:** insert a FAT32 card, run M5 Prom's SD Diagnostics, and finish any EEPROM/web operation before retrying.
- **Dependency installation failed:** check that Python 3.10+ is installed, internet access is available, and the machine can access PyPI. The batch launcher leaves an explanatory error on screen.
- **Recording fails:** ensure there is enough disk space and that your security software permits the bundled FFmpeg encoder.

Firmware runs standalone if the desktop software is not installed or the USB connection drops.

## Firmware flashing and USB recovery

**Important:** This build uses the ESP32-S3 USB-OTG controller, so the normal
Serial/JTAG COM port does **not** appear while M5 Prom firmware is running.
This is expected and is not a fault. **No eFuses are changed.**

To flash another firmware image, unplug USB, hold **BOOT/G0**, reconnect USB
(or press RESET while holding BOOT), then release BOOT. The ROM loader uses
the chip's download path; select the resulting COM port in the existing GUI
flasher, then flash your usual bootloader, partitions and application files.
If the device never enumerates, verify the cable, power and manual boot pins.
Do not use the Remote app at the same time as the firmware flasher.

**Engineering / distribution:** `0xCAFE:0x4055` is an *experimental* VID/PID
combination for development. Use an authorized product-specific USB VID/PID
before a public hardware/firmware release. The Windows WinUSB association may
need re-enumeration if IDs change. The MS OS 2.0 descriptor publishes a fixed
interface GUID so no manual Zadig step should be necessary on supported Windows.

USB Native Vendor requires managed ESP-IDF dependency `espressif/esp_tinyusb`
and `CONFIG_TINYUSB_VENDOR_COUNT=1`. In an existing project that has a generated
`sdkconfig`, confirm the TinyUSB values in `idf.py menuconfig`; `sdkconfig.defaults`
does not override an existing configuration. After switching from Serial/JTAG,
`idf.py fullclean` then `idf.py build` is recommended.

### Low-latency remote update (same firmware version)

The updated remote uses nonblocking USB enqueue for screen rows, checks pending
PC keyboard commands before processing incoming display data, and displays only
the latest complete screen at a maximum 30 FPS. Slow Qt repaint or a temporary
USB backlog therefore cannot cause a queue of outdated pictures to accumulate.
Video encoding also runs in a separate bounded worker, so a temporarily slow
MP4 writer does not freeze the keyboard or file manager. When overloaded the
recorder may drop older video frames; recording retains a nominal 12 FPS timebase.

These changes require matching firmware and desktop source for best results;
they do not change USB identifiers, partition offsets or the wire protocol.

## Pass 1 remote diagnostics (firmware v0.9.4)

Newer matching firmware displays two small diagnostic lines beneath the PC
mirror: actual completed-screen FPS, USB input KB/s, key-to-handler ACK p95,
**device-local** processing p95, and queue/heap information. Because unchanged
frames are skipped, a static menu may show 0 FPS without indicating a fault.
The optional status extension is disabled automatically with older firmware.

For a repeatable latency check, navigate menus slowly, then rapidly, noting
`Drops`, queue depth and the difference between host ACK timing and device UI
delay. These numbers help distinguish USB congestion from slow UI processing;
they are not yet a direct camera-verified key-to-visible-pixel measurement.
See [`../PASS1_USB_DIAGNOSTICS.md`](../PASS1_USB_DIAGNOSTICS.md) for the test
matrix and how to interpret each counter.

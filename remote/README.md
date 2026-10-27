# M5 Prom Remote

A single-window USB companion for **M5 Prom v0.9.4** running on the M5Stack Cardputer v1.1.

**No Wi-Fi, separate USB/UART adapter, or extra hardware is required.** Use the Cardputer's USB-C data cable. The PC app connects through the native USB-OTG vendor interface (WinUSB), not a serial COM port.

## Getting started on Windows

1. Install **Python 3.10 or newer**, if it isn't already installed.
2. Double-click **`remote/M5Prom-Remote.bat`**.
3. On first use, the launcher creates a private `.venv` folder inside `remote/` and installs any missing dependencies. It uses **unpinned package names** so pip selects the latest releases compatible with your Python. Internet access is required the first time.
4. Plug the Cardputer into the PC using a **data-capable** USB-C cable. Open M5 Prom firmware with **PC Mode support** (the accompanying patched source must be flashed first).
5. Select **M5 Prom Native USB** from the detected device list and click **Connect**.

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

## Troubleshooting

- **No M5 Prom USB device:** verify the updated native-USB firmware is flashed, reconnect the USB data cable, and check Device Manager. Windows 10/11 should auto-bind WinUSB using the included descriptors; allow device setup to finish. The old Serial/JTAG COM port will disappear while the app is running.
- **Connected but no picture:** install the supplied firmware additions. Old M5 Prom firmware does not speak the remote protocol. The app requires protocol v1.
- **SD unavailable/busy:** insert a FAT32 card, run M5 Prom's SD Diagnostics, and finish any EEPROM/web operation before retrying.
- **Dependency installation failed:** check that Python 3.10+ is installed, internet access is available, and the machine can access PyPI. The batch launcher leaves an explanatory error on screen.
- **Recording fails:** ensure there is enough disk space and that your security software permits the bundled FFmpeg encoder.

Firmware runs standalone if the desktop software is not installed or the USB connection drops.

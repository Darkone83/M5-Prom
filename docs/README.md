# M5 Prom web flasher

Static GitHub Pages installer for the original M5Stack Cardputer / v1.1.
The page entry point is `docs/index.html`. There is no website build step,
package installation, backend, or runtime CDN dependency.

## Enable GitHub Pages

1. Copy this package into the repository root, preserving `docs/` and `tools/`.
2. Commit the files to the branch you want GitHub Pages to use.
3. Open **Settings → Pages → Build and deployment**.
4. Select **Deploy from a branch**, choose that branch and **/docs**, and save.
5. Open the HTTPS address GitHub shows after the deployment completes.

For a project repository, the page is served at `https://OWNER.github.io/REPOSITORY/`.
Do not append `/docs/`. All site assets use relative URLs so project-page paths work.
The page's GitHub links detect the owner and repository from this URL. On a custom
domain, change the two `.repo-link` destinations in `index.html` to your repository.

The repository's root README is not included or edited. Paste this into it after
replacing `YOUR-REPOSITORY-NAME` with the actual repository name:

```md
## Web flasher

[Install M5 Prom from your browser](https://darkone83.github.io/YOUR-REPOSITORY-NAME/)

Use desktop Chrome or Edge and a USB-C data cable. Supports the original
M5Stack Cardputer / v1.1.
```

## Included firmware

These are the exact supplied binaries and `flasher_args.json`:

| Image | Offset | Bytes |
| --- | --- | ---: |
| `bootloader.bin` | `0x00000` | 20,848 |
| `partition-table.bin` | `0x08000` | 3,072 |
| `cardputer_xbox_eeprom.bin` | `0x10000` | 1,169,024 |

The image headers specify **ESP32-S3, DIO, 8 MB, 80 MHz**. The application
descriptor identifies version **1**, project `cardputer_xbox_eeprom`, built
**2026-10-27 14:51:44** with **ESP-IDF v5.5.5**. The site displays that actual
build date. These supplied binaries are not identified as the later RC1 source
patch; compile and replace the matched set when publishing that release.

The included factory partition starts at `0x10000` and is `0x177000` bytes
(1500 KiB). Do not mix this table with an application that exceeds it, or swap
individual files from different builds. The update helper and browser check
the application against the supplied partition table.

## Update firmware from a new build

From the repository root, run Python 3.9 or newer against the ESP-IDF build directory:

```sh
python3 tools/update_webflasher.py src/build --check
python3 tools/update_webflasher.py src/build
```

Use the actual build directory if it is different. It must contain
`flasher_args.json` and the three images referenced by that file, normally
`bootloader/bootloader.bin`, `partition_table/partition-table.bin`, and
`cardputer_xbox_eeprom.bin`.

The helper validates the matched set before changing the site. It copies exact
binary bytes and flash arguments, reads the application's real version/build
metadata, and generates `docs/firmware/manifest.json` with new lengths and
SHA-256 checksums. The manifest is written last. Commit all changed files in
`docs/firmware/` together, and deploy the whole commit. A visitor who receives
mixed old/new cached files is blocked by the integrity checks.

The helper intentionally requires the supported chip, flash settings, filenames,
and offsets. A different board or layout needs a deliberate installer change.

## Local preview and checks

Serve the site over localhost rather than opening the HTML as a file:

```sh
python3 -m http.server 8765 --directory docs
```

Open `http://localhost:8765/` in desktop Chrome or Edge. Public hosting must use
HTTPS. The responsive page remains readable in other browsers; installation is
enabled only when Web Serial is available.

Run the engine's host checks with a recent Node.js release:

```sh
node tools/test_webflasher.mjs
```

These checks simulate serial operations and failures. They do not replace a
physical flash on the supported Cardputer or confirm that a firmware build boots.

## Installation behavior

- The browser downloads the manifest and all three images, checks exact lengths
  and SHA-256 hashes, and validates image/partition metadata before offering a
  connection. It keeps those verified bytes for installation.
- Connect opens the browser's serial chooser directly from the user's click.
  Installation is a separate explicit button after device checks pass.
- The device must report ESP32-S3, at least 8 MB flash, a supported chip revision,
  an active RAM flashing stub, and a security state compatible with plaintext
  firmware. Chip/size detection cannot identify a Cardputer board: users must
  select the original Cardputer / v1.1, not ADV or another ESP32-S3 board.
- The installer writes only the three supplied ranges with no full-chip erase.
  It preserves the validated image header settings and checks each written image
  with the device's flash MD5 command. Success requires all three comparisons.
- The supplied erase ranges do not touch NVS or PHY data. This is not a promise
  that settings from another firmware or partition layout will remain compatible.
  The installer does not access microSD files or an Xbox EEPROM.
- A failed write stops the installation. It is not automatically retried, and
  the device is not automatically rebooted into potentially incomplete firmware.
- After verified success, a restart is requested. If the device does not start,
  press RESET without holding G0. A reset failure does not turn a verified flash
  into a failed-write result.
- Logs stay in the page and can be downloaded locally. No telemetry or firmware
  upload service is used.

If the Cardputer does not appear, close M5 Prom Remote and other serial programs.
Hold G0 / BOOT, tap RESET, release G0, then reconnect and select the newly exposed
USB serial port. The running M5 Prom Remote interface is not a ROM serial port.
Disconnect the Xbox adapter before updating the Cardputer firmware.

## Dependencies

`assets/vendor/esptool-js.js` bundles pinned esptool-js **0.7.1** and its browser
dependencies, plus SparkMD5 for device verification. Complete upstream license
notices, version information, and source hashes are in
`assets/vendor/NOTICE.txt`. Transport/reset adaptations are isolated in
`assets/flasher-core.js`.

## References

- [GitHub Pages publishing from a branch](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)
- [Espressif esptool-js](https://github.com/espressif/esptool-js)
- [M5Stack Cardputer v1.1 and download mode](https://docs.m5stack.com/en/core/Cardputer_v1.1)

Files to delete when applying this package: **none**.

#!/usr/bin/env python3
"""Validate one ESP-IDF build and refresh docs/firmware without rebuilding it.

Usage: python3 tools/update_webflasher.py path/to/build [--check]
       python3 tools/update_webflasher.py path/to/build --output-dir path/to/output

Uses only the Python standard library. The three binaries and flasher_args.json
are copied byte for byte. All checks finish before publication; manifest.json is
replaced last, so an interrupted update cannot authorize mismatched downloads.
This validates files and layout, not physical Cardputer operation.
"""

import argparse
from datetime import date, time
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile


FLASH_BYTES = 8 * 1024 * 1024
FILES = (
    ("bootloader", "Bootloader", "bootloader.bin", 0x00000, 0x8000),
    ("partitions", "Partition table", "partition-table.bin", 0x08000, 0x1000),
    ("application", "M5 Prom firmware", "cardputer_xbox_eeprom.bin", 0x10000, FLASH_BYTES - 0x10000),
)
SETTINGS = {"flash_mode": "dio", "flash_size": "8MB", "flash_freq": "80m"}


class InvalidBuild(ValueError):
    """An input is not a supported, internally consistent M5 Prom build."""


def require(condition, message):
    if not condition:
        raise InvalidBuild(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def field(data, offset, length):
    raw = data[offset:offset + length]
    require(len(raw) == length and b"\0" in raw, "Invalid or unterminated image descriptor field")
    value = raw.split(b"\0", 1)[0].decode("ascii")
    require(value and all(32 <= ord(char) < 127 for char in value), "Invalid image descriptor text")
    return value


def erase_end(offset, length):
    return (offset + length + 0xFFF) & ~0xFFF


def validate_image(data, name, flash_offset):
    require(len(data) >= 32, f"{name}: image is too short")
    magic, count, mode, size_freq, entry = struct.unpack_from("<BBBBI", data)
    chip = struct.unpack_from("<H", data, 12)[0]
    require(magic == 0xE9 and 1 <= count <= 16 and chip == 9,
            f"{name}: expected an ESP32-S3 image")
    require(mode == 2 and size_freq == 0x3F, f"{name}: expected DIO / 8MB / 80m")
    require(data[23] == 1, f"{name}: an appended SHA-256 image digest is required")
    minimum, maximum = struct.unpack_from("<HH", data, 15)
    require(minimum <= maximum, f"{name}: invalid chip revision range")
    cursor, checksum, first, entry_present = 24, 0xEF, None, False
    for _ in range(count):
        require(cursor + 8 <= len(data), f"{name}: truncated segment header")
        address, length = struct.unpack_from("<II", data, cursor)
        start, end = cursor + 8, cursor + 8 + length
        require(length % 4 == 0 and end <= len(data), f"{name}: invalid segment length")
        payload = data[start:end]
        if first is None:
            first = payload
        entry_present |= address <= entry < address + length
        if 0x3C000000 <= address < 0x3E000000 or 0x42000000 <= address < 0x44000000:
            require(address % 0x10000 == (flash_offset + start) % 0x10000,
                    f"{name}: flash-mapped segment is misaligned")
        for byte in payload:
            checksum ^= byte
        cursor = end
    checksum_at = cursor + 15 - cursor % 16
    require(checksum_at + 33 == len(data), f"{name}: unexpected footer, signature, or trailing data")
    require(data[cursor:checksum_at] == bytes(checksum_at - cursor), f"{name}: invalid image padding")
    require(data[checksum_at] == checksum, f"{name}: segment checksum mismatch")
    require(hashlib.sha256(data[:checksum_at + 1]).digest() == data[checksum_at + 1:],
            f"{name}: appended SHA-256 digest mismatch")
    require(entry_present, f"{name}: entry address is outside the image segments")
    return first, minimum, maximum


def application_metadata(first):
    require(len(first) >= 256 and struct.unpack_from("<I", first)[0] == 0xABCD5432,
            "The application has no valid ESP-IDF descriptor")
    require(field(first, 48, 32) == "cardputer_xbox_eeprom", "The application is not M5 Prom")
    month_text, day_text, year_text = field(first, 96, 16).split()
    months = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    require(month_text in months, "The application build month is invalid")
    build_date = date(int(year_text), months.index(month_text) + 1, int(day_text)).isoformat()
    build_time = field(first, 80, 16)
    require(time.fromisoformat(build_time).isoformat() == build_time, "The application build time is invalid")
    return {"version": field(first, 16, 32), "buildDate": build_date,
            "buildTime": build_time, "idfVersion": field(first, 112, 32)}


def validate_partitions(data, images):
    require(len(data) % 32 == 0, "Partition table length must be a multiple of 32")
    parts, position = [], 0
    while position + 32 <= len(data) and data[position:position + 2] == b"\xAA\x50":
        _, kind, subtype, offset, size, label, flags = struct.unpack_from("<HBBII16sI", data, position)
        require(size > 0 and offset >= 0x9000 and offset % 0x1000 == size % 0x1000 == 0
                and offset + size <= FLASH_BYTES, "Invalid partition bounds or alignment")
        require(kind != 0 or offset % 0x10000 == 0, "App partition must have 64 KiB alignment")
        require(flags == 0, "Encrypted or flagged partitions need a separate release review")
        require(not any(offset < old[2] + old[3] and old[2] < offset + size for old in parts),
                "Partition ranges overlap")
        parts.append((kind, subtype, offset, size))
        position += 32
    require(data[position:position + 16] == b"\xEB\xEB" + b"\xFF" * 14,
            "The partition table MD5 record is missing")
    require(hashlib.md5(data[:position]).digest() == data[position + 16:position + 32],
            "Partition table MD5 mismatch")
    tail = data[position + 32:]
    require(len(tail) >= 32 and tail == b"\xFF" * len(tail), "Invalid partition table end marker or padding")
    factories = [part for part in parts if part[:2] == (0, 0)]
    require(len(factories) == 1 and factories[0][2] == 0x10000, "Expected one factory app at 0x10000")
    require((1, 2, 0x9000, 0x6000) in parts and (1, 1, 0xF000, 0x1000) in parts,
            "Expected NVS at 0x9000 (24 KiB) and PHY at 0xF000 (4 KiB)")
    factory = factories[0]
    require(erase_end(0x10000, len(images["cardputer_xbox_eeprom.bin"])) <= factory[2] + factory[3],
            "The application does not fit its factory partition")
    for _, _, filename, offset, _ in FILES:
        end = erase_end(offset, len(images[filename]))
        require(not any(kind != 0 and offset < start + size and start < end
                        for kind, _, start, size in parts),
                f"{filename}: erase sectors overlap a data partition")
    return factory[3]


def load_build(build_dir):
    build_dir = build_dir.resolve(strict=True)
    args_data = (build_dir / "flasher_args.json").read_bytes()
    require(len(args_data) <= 65536, "flasher_args.json is unexpectedly large")
    args = json.loads(args_data, object_pairs_hook=unique_object)
    require(isinstance(args, dict), "flasher_args.json must contain an object")
    require(args.get("flash_settings") == SETTINGS, "Expected DIO / 8MB / 80m flash settings")
    write_args = args.get("write_flash_args")
    require(isinstance(write_args, list) and len(write_args) == 6 and all(isinstance(v, str) for v in write_args),
            "Unexpected write_flash_args")
    pairs = [(write_args[i].removeprefix("--").replace("-", "_"), write_args[i + 1]) for i in range(0, 6, 2)]
    require(len(dict(pairs)) == 3 and dict(pairs) == SETTINGS, "write_flash_args disagree with flash_settings")
    extra = args.get("extra_esptool_args")
    require(isinstance(extra, dict) and extra.get("chip") == "esp32s3" and extra.get("stub") is True
            and extra.get("before") == "default_reset" and extra.get("after") == "hard_reset",
            "Expected ESP32-S3, RAM stub, default_reset, and hard_reset build arguments")
    flash_files = args.get("flash_files")
    require(isinstance(flash_files, dict) and len(flash_files) == 3, "Expected exactly three flash_files")
    offsets = {}
    for offset, filename in flash_files.items():
        require(isinstance(offset, str) and isinstance(filename, str), "Invalid flash_files entry")
        number = int(offset, 0)
        require(number not in offsets, "Duplicate numeric flash offset")
        offsets[number] = filename
    require(set(offsets) == {0, 0x8000, 0x10000}, "Expected flash offsets 0x0, 0x8000, and 0x10000")
    for section, offset in (("bootloader", 0), ("partition-table", 0x8000), ("app", 0x10000)):
        entry = args.get(section)
        require(isinstance(entry, dict) and entry.get("file") == offsets[offset]
                and int(entry.get("offset", "-1"), 0) == offset
                and (entry.get("encrypted") is False or entry.get("encrypted") == "false"),
                f"Inconsistent or encrypted {section} build entry")
    images, manifest_images = {}, []
    for role, name, filename, offset, maximum in FILES:
        source = (build_dir / offsets[offset]).resolve(strict=True)
        require(source.is_relative_to(build_dir), "All image inputs must belong to the same build directory")
        require(32 <= source.stat().st_size <= maximum, f"{filename}: file length exceeds its flash range")
        data = source.read_bytes()
        images[filename] = data
        manifest_images.append({"name": name, "file": filename, "role": role, "offset": offset,
                                "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    boot, boot_min, boot_max = validate_image(images["bootloader.bin"], "bootloader.bin", 0)
    app, app_min, app_max = validate_image(images["cardputer_xbox_eeprom.bin"], "cardputer_xbox_eeprom.bin", 0x10000)
    require(max(boot_min, app_min) <= min(boot_max, app_max), "Bootloader and app chip revision ranges do not overlap")
    metadata = application_metadata(app)
    require(len(boot) >= 80 and boot[0] == 80, "The bootloader has no valid ESP-IDF descriptor")
    require(field(boot, 8, 32) == metadata["idfVersion"], "Bootloader and application ESP-IDF versions differ")
    factory_size = validate_partitions(images["partition-table.bin"], images)
    manifest = {"schemaVersion": 1, "name": "M5 Prom", "releaseLabel": f"Build {metadata['buildDate']}",
                **metadata, "chip": "ESP32-S3", "flashMode": "dio", "flashSize": "8MB", "flashFreq": "80m",
                "images": manifest_images}
    images["flasher_args.json"] = args_data
    return images, manifest, factory_size


def publish(output_dir, images, manifest):
    output_dir = output_dir.resolve()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="m5-prom-web-", dir=output_dir.parent) as staging_name:
        staging = Path(staging_name)
        for filename, data in images.items():
            (staging / filename).write_bytes(data)
        (staging / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        output_dir.mkdir(parents=True, exist_ok=True)
        for filename in images:
            os.replace(staging / filename, output_dir / filename)
        os.replace(staging / "manifest.json", output_dir / "manifest.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", type=Path, help="ESP-IDF build directory containing flasher_args.json")
    parser.add_argument("--check", action="store_true", help="validate inputs without writing any output")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[1] / "docs" / "firmware")
    options = parser.parse_args()
    try:
        images, manifest, factory_size = load_build(options.build_dir)
        if not options.check:
            publish(options.output_dir, images, manifest)
    except (OSError, ValueError, TypeError, KeyError, struct.error) as error:
        print(f"Web flasher update refused: {error}", file=sys.stderr)
        return 1
    print(f"Validated {manifest['releaseLabel']} {manifest['buildTime']}; app version {manifest['version']}; {manifest['idfVersion']}.")
    print(f"Factory partition: {len(images['cardputer_xbox_eeprom.bin']):,} / {factory_size:,} bytes. NVS and PHY erase ranges are untouched.")
    for image in manifest["images"]:
        print(f"0x{image['offset']:05X}  {image['size']:>9} bytes  {image['file']}  SHA-256 {image['sha256']}")
    print("Validation only; no files written." if options.check else f"Published matched files to {options.output_dir.resolve()} (manifest last).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

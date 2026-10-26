"""Small non-GUI helper for M5 Prom's three-file graphical updater.

The operator chooses each file separately; no directories are searched or
file names assumed. Contains no serial or device operations.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

# ESP-IDF 5.5.x, Cardputer ESP32-S3, M5 Prom factory-image layout.
OFFSETS = {"bootloader": 0x00000, "partitions": 0x08000, "application": 0x10000}
FLASH_SIZE = 0x800000  # Original M5 Prom Cardputer V1.1: 8 MB


class FirmwareError(ValueError):
    """An invalid or missing selected firmware image."""


@dataclass(frozen=True)
class Image:
    role: str
    path: Path
    offset: int
    length: int


@dataclass(frozen=True)
class Firmware:
    bootloader: Image
    partitions: Image
    application: Image

    @property
    def images(self) -> tuple[Image, Image, Image]:
        return self.bootloader, self.partitions, self.application


def _factory_partition(path: Path) -> tuple[int, int]:
    """Read factory app offset/size from an ESP-IDF partition table."""
    data = path.read_bytes()
    factory = []
    for pos in range(0, len(data) - 31, 32):
        entry = data[pos:pos + 32]
        magic = struct.unpack_from('<H', entry)[0]
        if magic == 0xffff:
            break
        if magic == 0xebeb:  # MD5 record
            continue
        if magic != 0x50aa:
            raise FirmwareError("Partition table file is not a valid ESP-IDF .bin file.")
        _, kind, subtype, offset, size, _, _ = struct.unpack('<HBBII16sI', entry)
        if size == 0 or offset < 0x9000 or offset + size > FLASH_SIZE:
            raise FirmwareError("Partition table contains an invalid flash address or size.")
        if kind == 0 and subtype == 0:
            factory.append((offset, size))
    if len(factory) != 1:
        raise FirmwareError("Partition table must contain a factory application partition.")
    return factory[0]


def firmware_from_files(bootloader: str | Path, partitions: str | Path,
                        application: str | Path) -> Firmware:
    """Validate three explicitly selected images, regardless of name/folder."""
    paths = {
        "bootloader": Path(bootloader).expanduser().resolve(),
        "partitions": Path(partitions).expanduser().resolve(),
        "application": Path(application).expanduser().resolve(),
    }
    if len(set(paths.values())) != 3:
        raise FirmwareError("Select three different files: bootloader, partition table and firmware.")
    for role, path in paths.items():
        if not path.is_file():
            raise FirmwareError(f"Can't open selected {role} file: {path.name}")

    images = {}
    for role, path in paths.items():
        size = path.stat().st_size
        limit = {"bootloader": 0x8000, "partitions": 0x1000,
                 "application": FLASH_SIZE - OFFSETS["application"]}[role]
        if size < 32 or size > limit:
            raise FirmwareError(f"The selected {role} file has an unexpected size.")
        with path.open('rb') as reader:
            header = reader.read(2)
        expected = b'\xaa\x50' if role == "partitions" else b'\xe9'
        if not header.startswith(expected):
            raise FirmwareError(
                f"The selected {role} file doesn't look like the correct ESP32-S3 binary."
            )
        images[role] = Image(role, path, OFFSETS[role], size)

    factory_offset, factory_size = _factory_partition(paths["partitions"])
    if factory_offset != OFFSETS["application"]:
        raise FirmwareError("The selected partition table does not place firmware at 0x10000.")
    if images["application"].length > factory_size:
        raise FirmwareError("Firmware is larger than the space allowed by this partition table.")
    return Firmware(**images)


def flash_command(firmware: Firmware, port: str, python_exe: str,
                  esptool_major: int, baud: int = 460800) -> list[str]:
    if not port or any(c in port for c in '\r\n\x00'):
        raise FirmwareError("Choose a valid USB serial port.")
    if baud not in (115200, 460800):
        raise FirmwareError("Unsupported baud rate.")
    verb = 'write-flash' if esptool_major >= 5 else 'write_flash'
    command = [python_exe, '-u', '-m', 'esptool', '--chip', 'esp32s3',
               '--port', port, '--baud', str(baud), verb]
    for image in firmware.images:
        command.extend([f'0x{image.offset:x}', str(image.path)])
    return command

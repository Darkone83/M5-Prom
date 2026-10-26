"""File-selection and flashing command tests (no Cardputer required)."""
from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flasher_core import FirmwareError, firmware_from_files, flash_command


def partition_table(offset=0x10000, size=0x300000) -> bytes:
    def record(typ, subtype, start, length, title):
        return struct.pack('<HBBII16sI', 0x50AA, typ, subtype, start, length,
                           title.encode().ljust(16, b'\x00'), 0)
    return (record(1, 2, 0x9000, 0x6000, 'nvs') +
            record(1, 1, 0xf000, 0x1000, 'phy_init') +
            record(0, 0, offset, size, 'factory') + b'\xff' * (0x1000 - 96))


class UserSelectedFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        # Deliberately unrelated directories and filenames.
        self.boot = base / 'Downloads' / 'first.bin'
        self.part = base / 'Downloads' / 'release' / 'second.bin'
        self.app = base / 'Desktop' / 'third.bin'
        for item in (self.boot, self.part, self.app):
            item.parent.mkdir(parents=True, exist_ok=True)
        self.boot.write_bytes(b'\xe9\x02' + b'\x00' * 98)
        self.part.write_bytes(partition_table())
        self.app.write_bytes(b'\xe9\x07' + b'\x00' * 2048)

    def fw(self):
        return firmware_from_files(self.boot, self.part, self.app)

    def test_files_can_be_named_anything_in_different_folders(self):
        fw = self.fw()
        self.assertEqual([i.path for i in fw.images], [self.boot, self.part, self.app])
        self.assertEqual([i.offset for i in fw.images], [0, 0x8000, 0x10000])

    def test_missing_file(self):
        self.app.unlink()
        with self.assertRaisesRegex(FirmwareError, "Can't open"):
            self.fw()

    def test_same_file_twice(self):
        with self.assertRaisesRegex(FirmwareError, 'three different'):
            firmware_from_files(self.boot, self.part, self.boot)

    def test_wrong_bin_type(self):
        self.boot.write_bytes(b'\x00' * 64)
        with self.assertRaisesRegex(FirmwareError, "doesn't look"):
            self.fw()

    def test_bad_partition_table(self):
        self.part.write_bytes(b'\x00' * 200)
        with self.assertRaisesRegex(FirmwareError, 'doesn.t look'):
            self.fw()

    def test_incompatible_partition_offset(self):
        self.part.write_bytes(partition_table(offset=0x20000))
        with self.assertRaisesRegex(FirmwareError, '0x10000'):
            self.fw()

    def test_app_must_fit_factory_slot(self):
        self.part.write_bytes(partition_table(size=0x3000))
        with self.app.open('ab') as file:
            file.truncate(0x3001)
        with self.assertRaisesRegex(FirmwareError, 'larger than'):
            self.fw()

    def test_partition_bounds(self):
        self.part.write_bytes(partition_table(size=0x900000))
        with self.assertRaisesRegex(FirmwareError, 'invalid flash'):
            self.fw()

    def test_bootloader_max_size(self):
        with self.boot.open('ab') as file:
            file.truncate(0x8001)
        with self.assertRaisesRegex(FirmwareError, 'unexpected size'):
            self.fw()

    def test_flash_commands_three_images_no_erasure(self):
        for major, verb in ((4, 'write_flash'), (5, 'write-flash')):
            command = flash_command(self.fw(), 'COM5', 'python', major)
            self.assertIn(verb, command)
            self.assertEqual(command[command.index(verb) + 1::2],
                             ['0x0', '0x8000', '0x10000'])
            self.assertNotIn('erase_flash', command)
            self.assertNotIn('erase-flash', command)

    def test_empty_port_rejected(self):
        with self.assertRaises(FirmwareError):
            flash_command(self.fw(), '', 'python', 5)

    def test_nonexistent_bin_not_created(self):
        missing = self.app.parent / 'notreal.bin'
        with self.assertRaises(FirmwareError):
            firmware_from_files(self.boot, self.part, missing)
        self.assertFalse(missing.exists())


if __name__ == '__main__':
    unittest.main()

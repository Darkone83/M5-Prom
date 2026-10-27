"""M5 Prom USB vendor interface (WinUSB/libusb), no COM port, no UART.

The firmware uses temporary prototype VID/PID values. Assign product-owned
USB IDs before shipping outside a controlled development environment.
"""
from __future__ import annotations

from dataclasses import dataclass
import usb.core
import usb.util
import libusb_package

VID, PID = 0xCAFE, 0x4055
INTERFACE = 0
EP_OUT, EP_IN = 0x01, 0x81


@dataclass(frozen=True)
class DeviceInfo:
    identifier: str
    label: str


def _backend():
    return libusb_package.get_libusb1_backend()


def available_devices() -> list[DeviceInfo]:
    backend = _backend()
    if backend is None:
        raise RuntimeError("USB library unavailable; restart the launcher to install libusb-package")
    result = []
    for index, dev in enumerate(usb.core.find(find_all=True, idVendor=VID, idProduct=PID, backend=backend)):
        bus = getattr(dev, "bus", None)
        address = getattr(dev, "address", None)
        key = f"{bus}:{address}" if bus is not None and address is not None else f"device:{index}"
        result.append(DeviceInfo(key, f"M5 Prom Native USB · {key}"))
    return result


class NativeUSB:
    """Byte stream adaptor over vendor bulk endpoints, for the v1 framed protocol."""
    def __init__(self, identifier: str):
        backend = _backend()
        if backend is None:
            raise RuntimeError("libusb backend unavailable")
        devices = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID, backend=backend))
        match = None
        for index, dev in enumerate(devices):
            bus, address = getattr(dev, "bus", None), getattr(dev, "address", None)
            key = f"{bus}:{address}" if bus is not None and address is not None else f"device:{index}"
            if key == identifier:
                match = dev
                break
        if match is None:
            raise OSError("M5 Prom not found. Reconnect USB and refresh devices.")
        self.dev = match
        self.is_open = False
        try:
            self.dev.set_configuration()
            # Only a single vendor interface. Windows automatically binds WinUSB
            # via the firmware's MS OS 2.0 descriptors; no Zadig is expected.
            usb.util.claim_interface(self.dev, INTERFACE)
            self.is_open = True
        except usb.core.USBError as exc:
            usb.util.dispose_resources(self.dev)
            raise OSError("Cannot open M5 Prom USB interface. Reconnect the device and check Windows Device Manager (WinUSB).") from exc

    def read(self, size: int) -> bytes:
        if not self.is_open:
            raise OSError("USB interface is closed")
        try:
            return bytes(self.dev.read(EP_IN, min(size, 8192), timeout=5))
        except usb.core.USBTimeoutError:
            return b""
        except usb.core.USBError as exc:
            raise OSError(f"USB read error: {exc}") from exc

    def write(self, buf: bytes) -> int:
        if not self.is_open:
            raise OSError("USB interface is closed")
        done = 0
        try:
            while done < len(buf):
                amount = self.dev.write(EP_OUT, buf[done:], timeout=250)
                if amount <= 0:
                    raise OSError("Native USB write stalled")
                done += amount
        except usb.core.USBError as exc:
            raise OSError(f"USB write error: {exc}") from exc
        return done

    def close(self):
        if self.is_open:
            self.is_open = False
            try:
                usb.util.release_interface(self.dev, INTERFACE)
            finally:
                usb.util.dispose_resources(self.dev)

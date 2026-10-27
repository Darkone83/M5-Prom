"""First-run dependency installer, no pinned or upper-bounded package versions.

Launch with a Python interpreter in an isolated environment. Installs only
missing dependencies, using pip's latest compatible release resolution.
"""
from __future__ import annotations
import importlib
import subprocess
import sys
from pathlib import Path

NEEDED = (
    ("PySide6.QtWidgets", "PySide6-Essentials"),
    ("usb.core", "pyusb"),
    ("libusb_package", "libusb-package"),
    ("numpy", "numpy"),
    ("imageio", "imageio"),
    ("imageio_ffmpeg", "imageio-ffmpeg"),
)


def missing_packages() -> list[str]:
    missing = []
    for module, package in NEEDED:
        try:importlib.import_module(module)
        except ImportError:missing.append(package)
    return missing


def install_missing() -> None:
    if sys.version_info < (3, 10):
        raise RuntimeError("Python 3.10 or newer is required.")
    needed = missing_packages()
    if not needed:
        print("M5 Prom Remote: dependencies already installed.")
        return
    print("Installing missing M5 Prom Remote components (latest compatible releases):")
    print("  " + ", ".join(needed))
    subprocess.check_call([sys.executable,"-m","pip","install",
                           "--disable-pip-version-check",*needed])
    if missing_packages():
        raise RuntimeError("Some required packages are still unavailable after installation.")


def main() -> int:
    install_missing()
    script=Path(__file__).resolve().parent/"remote_app.py"
    print("Opening M5 Prom Remote...")
    # Keep console startup errors visible if the GUI cannot initialize.
    return subprocess.call([sys.executable,str(script)],cwd=str(script.parent))


if __name__ == "__main__":
    try:sys.exit(main())
    except (RuntimeError,subprocess.CalledProcessError) as exc:
        print(f"\nSETUP ERROR: {exc}",file=sys.stderr)
        sys.exit(1)

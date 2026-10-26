#!/usr/bin/env python3
"""M5 Prom GUI starter: prepares an isolated environment on first launch.

Run this file or double-click tools/M5Prom-Flasher.bat on Windows. First-run
setup uses Python's own venv/pip so it does not modify any ESP-IDF install.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import venv

HERE = Path(__file__).resolve().parent
ENV_DIR = HERE / ".flasher-env"
READY = ENV_DIR / ".ready"
PACKAGES = ("PySide6-Essentials", "esptool")  # Resolve newest compatible stable releases at setup


def _python_in_venv() -> Path:
    if os.name == "nt":
        return ENV_DIR / "Scripts" / "python.exe"
    return ENV_DIR / "bin" / "python"


def _have_dependencies() -> bool:
    try:
        import PySide6.QtWidgets  # noqa: F401
        import esptool  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
        return True
    except (ImportError, OSError):
        return False


def _message(title: str, message: str) -> None:
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, message, title, 0x10)
        except Exception:
            pass
    print(f"\n{title}: {message}", file=sys.stderr, flush=True)


def _setup() -> int:
    if sys.version_info < (3, 10):
        _message("Python update needed", "M5 Prom Flasher requires Python 3.10 or newer.\nVisit python.org/downloads to install it.")
        return 1
    try:
        if not _python_in_venv().is_file():
            print("Preparing M5 Prom Flasher for the first time...", flush=True)
            venv.EnvBuilder(with_pip=True, clear=False).create(str(ENV_DIR))
        python = _python_in_venv()
        check_cmd = [str(python), "-c", "import PySide6.QtWidgets, esptool, serial.tools.list_ports"]
        if subprocess.run(check_cmd, capture_output=True, check=False).returncode != 0:
            READY.unlink(missing_ok=True)
            print("Installing the graphical interface and USB flashing support...", flush=True)
            print("An internet connection is needed for initial setup.", flush=True)
            # Do not use --only-binary=:all:. esptool is distributed as an sdist.
            cmd = [str(python), "-m", "pip", "--disable-pip-version-check", "--no-input", "install", "--upgrade", *PACKAGES]
            result = subprocess.run(cmd, check=False)
            if result.returncode != 0:
                _message("Setup couldn't finish", "Could not download the required packages. Check your internet connection and try opening M5Prom-Flasher again.")
                return 1
            if subprocess.run(check_cmd, capture_output=True, check=False).returncode != 0:
                _message("Setup incomplete", "Required packages did not load after installation. Reopen the launcher to retry.")
                return 1
        READY.touch()
        return subprocess.call([str(python), str(Path(__file__).resolve()), "--run-gui"])
    except (OSError, subprocess.SubprocessError) as exc:
        _message("Setup failed", f"Could not prepare the flasher:\n{exc}\nTry reopening the launcher.")
        return 1


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--run-gui":
        if not _have_dependencies():
            READY.unlink(missing_ok=True)
            _message("Missing components", "The flasher's installation needs repair. Double-click M5Prom-Flasher again to reinstall missing parts.")
            return 1
        from flasher_gui import run_gui
        return run_gui()
    if sys.argv[1:]:
        print("M5 Prom Flasher is graphical. Start with no arguments.")
        return 2
    return _setup()


if __name__ == "__main__":
    raise SystemExit(main())

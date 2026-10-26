"""M5 Prom USB firmware flasher: choose three files, select port, click Flash."""
from __future__ import annotations

import importlib.metadata
from pathlib import Path
import re
import sys

from PySide6.QtCore import QProcess, QProcessEnvironment, QTimer, Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFileDialog, QFrame, QHBoxLayout, QLabel,
    QMainWindow, QMessageBox, QPushButton, QProgressBar, QTextEdit,
    QVBoxLayout, QWidget,
)
from serial.tools import list_ports

from flasher_core import Firmware, FirmwareError, firmware_from_files, flash_command

ROOT = Path(__file__).resolve().parent.parent
ROLES = (("bootloader", "Bootloader"),
         ("partitions", "Partitions"),
         ("application", "Firmware"))

STYLE = """
QWidget {background: #100e1a; color: #f7f3ff; font: 12px 'Segoe UI';}
QLabel#Title {font-size: 25px; font-weight: 750;}
QLabel#Muted {color: #b0a7c6;}
QLabel#Field {font-weight: 700; color: #e7ddff;}
QFrame#Panel {background: #1e1a2b; border: 1px solid #383049; border-radius: 12px;}
QPushButton {background: #322947; border: 1px solid #605278; border-radius: 7px;
             padding: 8px 12px; font-weight: 600;}
QPushButton:hover {background: #44355f;}
QPushButton:disabled {background: #282331; color: #777084; border-color: #383342;}
QPushButton#Flash {background: #875ad1; border: 1px solid #a783e0;
                   font-size: 16px; font-weight: 750; padding: 14px;}
QPushButton#Flash:hover {background: #9a6ae2;}
QPushButton#Flash:disabled {background: #392e4b; border-color: #463955;}
QPushButton#Link {border: 0; background: transparent; color: #d4bfff;}
QComboBox {background: #292338; border: 1px solid #524469; border-radius: 7px;
           padding: 8px; min-height: 19px;}
QComboBox QAbstractItemView {background: #292338; selection-background-color: #7153a8;}
QProgressBar {background: #29233a; border: 0; border-radius: 6px; height: 19px;
              text-align: center; color: white; font-weight: 700;}
QProgressBar::chunk {background: #9e74e0; border-radius: 6px;}
QTextEdit {background: #151020; border: 1px solid #433553; border-radius: 7px;
           color: #c9bcde; font: 11px Consolas;}
"""


def make_label(text: str, kind: str = "") -> QLabel:
    item = QLabel(text)
    if kind:
        item.setObjectName(kind)
    return item


def panel() -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setObjectName("Panel")
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(19, 17, 19, 18)
    layout.setSpacing(12)
    return frame, layout


class Flasher(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("M5 Prom — Firmware Flasher")
        self.setMinimumSize(610, 510)
        self.resize(730, 660)
        self.setStyleSheet(STYLE)
        self.selected: dict[str, Path | None] = {role: None for role, _ in ROLES}
        self.last_browse_dir = Path.home()
        self.firmware: Firmware | None = None
        self.busy = False
        self.process: QProcess | None = None
        self.log = ""
        self.tail = ""
        self.verified_count = 0
        self._build_ui()
        self._refresh_ports()
        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self._refresh_ports)
        self.refresh_timer.start(4000)
        self._update_ready()

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(23, 20, 23, 20)
        layout.setSpacing(15)

        header = QHBoxLayout()
        left = QVBoxLayout()
        logo = ROOT / "img" / "logo.png"
        if logo.is_file():
            pix = QPixmap(str(logo))
            if not pix.isNull():
                picture = QLabel()
                picture.setPixmap(pix.scaled(220, 58, Qt.AspectRatioMode.KeepAspectRatio,
                                             Qt.TransformationMode.SmoothTransformation))
                left.addWidget(picture)
            else:
                left.addWidget(make_label("M5 PROM", "Title"))
        else:
            left.addWidget(make_label("M5 PROM", "Title"))
        left.addWidget(make_label("Firmware Flasher · Plug in, choose files, flash.", "Muted"))
        header.addLayout(left, 1)
        brand = ROOT / "img" / "darkone83.png"
        if brand.is_file():
            pix = QPixmap(str(brand))
            if not pix.isNull():
                brand_label = QLabel()
                brand_label.setPixmap(pix.scaled(103, 55, Qt.AspectRatioMode.KeepAspectRatio,
                                                 Qt.TransformationMode.SmoothTransformation))
                header.addWidget(brand_label)
        layout.addLayout(header)

        device_box, device_layout = panel()
        device_layout.addWidget(make_label("1.  Select your Cardputer USB port", "Field"))
        port_row = QHBoxLayout()
        self.port_box = QComboBox()
        self.port_box.currentIndexChanged.connect(self._update_ready)
        port_row.addWidget(self.port_box, 1)
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.clicked.connect(lambda: self._refresh_ports(force=True))
        port_row.addWidget(self.refresh_button)
        device_layout.addLayout(port_row)
        layout.addWidget(device_box)

        files_box, files_layout = panel()
        files_layout.addWidget(make_label("2.  Choose your three .bin files", "Field"))
        files_layout.addWidget(make_label("They can be anywhere on your computer; no special folder or names required.", "Muted"))
        self.file_labels: dict[str, QLabel] = {}
        self.browse_buttons: list[QPushButton] = []
        for role, title in ROLES:
            row = QHBoxLayout()
            field_name = make_label(title, "Field")
            field_name.setFixedWidth(83)
            row.addWidget(field_name)
            selected = make_label("Not selected", "Muted")
            selected.setMinimumWidth(1)
            selected.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            self.file_labels[role] = selected
            row.addWidget(selected, 1)
            browse = QPushButton("Browse…")
            browse.clicked.connect(lambda _checked=False, r=role: self._browse(r))
            self.browse_buttons.append(browse)
            row.addWidget(browse)
            files_layout.addLayout(row)
        layout.addWidget(files_box)

        install_box, install_layout = panel()
        install_layout.addWidget(make_label("3.  Flash M5 Prom", "Field"))
        self.flash_btn = QPushButton("Flash")
        self.flash_btn.setObjectName("Flash")
        self.flash_btn.setMinimumHeight(48)
        self.flash_btn.clicked.connect(self._flash)
        install_layout.addWidget(self.flash_btn)
        self.status = make_label("Choose your files and USB port to begin.", "Muted")
        self.status.setWordWrap(True)
        install_layout.addWidget(self.status)
        self.progress = QProgressBar()
        self.progress.setValue(0)
        install_layout.addWidget(self.progress)
        layout.addWidget(install_box)

        row = QHBoxLayout()
        self.help_btn = QPushButton("Connection help")
        self.help_btn.setObjectName("Link")
        self.help_btn.clicked.connect(self._help)
        row.addWidget(self.help_btn)
        row.addStretch(1)
        self.details_btn = QPushButton("Show details")
        self.details_btn.setObjectName("Link")
        self.details_btn.clicked.connect(self._toggle_details)
        row.addWidget(self.details_btn)
        layout.addLayout(row)
        self.details = QTextEdit()
        self.details.setReadOnly(True)
        self.details.setMaximumHeight(150)
        self.details.hide()
        layout.addWidget(self.details)
        layout.addStretch(1)
        layout.addWidget(make_label("M5 Prom · Darkone83 Customs · ESP32-S3", "Muted"))

    def _browse(self, role: str) -> None:
        candidate, _ = QFileDialog.getOpenFileName(
            self, f"Select {role} .bin", str(self.last_browse_dir),
            "Firmware files (*.bin);;All files (*)",
        )
        if not candidate:
            return
        chosen = Path(candidate)
        self.last_browse_dir = chosen.parent
        self.selected[role] = chosen
        label = self.file_labels[role]
        label.setText(chosen.name)
        label.setToolTip(str(chosen))
        label.setStyleSheet("color: #bcebd5;")
        self._update_ready()

    def _refresh_ports(self, force: bool = False) -> None:
        if self.busy or (self.port_box.view().isVisible() and not force):
            return
        current = self.port_box.currentData()
        try:
            ports = list(list_ports.comports())
        except (OSError, ValueError):
            ports = []
        ports.sort(key=lambda port: port.device.casefold())
        items = [(p.device, f"{p.device}  ·  {p.description or 'USB serial device'}") for p in ports]
        existing = [(self.port_box.itemData(i), self.port_box.itemText(i)) for i in range(self.port_box.count())]
        if items == existing:
            return
        self.port_box.blockSignals(True)
        self.port_box.clear()
        for dev, desc in items:
            self.port_box.addItem(desc, dev)
        ids = [dev for dev, _ in items]
        if current in ids:
            self.port_box.setCurrentIndex(ids.index(current))
        elif len(ids) == 1:
            self.port_box.setCurrentIndex(0)
        else:
            self.port_box.setCurrentIndex(-1)
            self.port_box.setPlaceholderText("Select USB port" if ids else "No USB device found")
        self.port_box.blockSignals(False)
        self._update_ready()

    def _update_ready(self, *_args) -> None:
        self.firmware = None
        if all(self.selected.values()):
            try:
                self.firmware = firmware_from_files(
                    self.selected["bootloader"], self.selected["partitions"],
                    self.selected["application"],
                )
            except (FirmwareError, OSError) as error:
                if not self.busy:
                    self.status.setText(str(error))
                    self.status.setStyleSheet("color:#ffb0a6;")
        if not self.busy:
            if not all(self.selected.values()):
                self.status.setText("Select the Bootloader, Partitions and Firmware files.")
                self.status.setStyleSheet("")
            elif self.firmware and not self.port_box.currentData():
                self.status.setText("Choose your Cardputer USB port.")
                self.status.setStyleSheet("")
            elif self.firmware:
                self.status.setText("Ready to flash.")
                self.status.setStyleSheet("color:#bcebd5;")
            self.flash_btn.setEnabled(bool(self.firmware and self.port_box.currentData()))

    def _help(self) -> None:
        QMessageBox.information(
            self, "Connect Cardputer",
            "Connect the Cardputer to your PC with a USB-C data cable.\n\n"
            "If it doesn't show up or won't connect:\n"
            "1. Disconnect the Xbox adapter.\n"
            "2. Turn the Cardputer power switch OFF.\n"
            "3. Hold G0 while connecting USB-C to your PC.\n"
            "4. Release G0, then click Refresh.\n\n"
            "Keep the USB cable connected until flashing finishes.",
        )

    def _toggle_details(self) -> None:
        show = not self.details.isVisible()
        self.details.setVisible(show)
        self.details_btn.setText("Hide details" if show else "Show details")

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        self.port_box.setEnabled(not busy)
        self.refresh_button.setEnabled(not busy)
        for button in self.browse_buttons:
            button.setEnabled(not busy)
        if busy:
            self.flash_btn.setEnabled(False)
            self.flash_btn.setText("Flashing…")
        else:
            self.flash_btn.setText("Flash")
            self._update_ready()

    def _flash(self) -> None:
        if not self.firmware or not self.port_box.currentData() or self.busy:
            return
        files = "\n".join(f"{title}: {getattr(self.firmware, role).path.name}" for role, title in ROLES)
        result = QMessageBox.question(
            self, "Flash M5 Prom?",
            f"Flash these three files to {self.port_box.currentData()}?\n\n{files}\n\n"
            "Keep USB connected until completed. No full-chip erase will be done.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if result != QMessageBox.StandardButton.Yes:
            return
        try:
            major = int(importlib.metadata.version('esptool').split('.', 1)[0])
            interpreter = sys.executable
            if Path(interpreter).name.casefold() == 'pythonw.exe':
                interpreter = str(Path(interpreter).with_name('python.exe'))
            cmd = flash_command(self.firmware, str(self.port_box.currentData()), interpreter, major)
        except (FirmwareError, ValueError, importlib.metadata.PackageNotFoundError) as error:
            QMessageBox.warning(self, "Cannot flash", str(error))
            return
        self.log = ""
        self.tail = ""
        self.details.clear()
        self.verified_count = 0
        self._set_busy(True)
        self.status.setText("Connecting to Cardputer…")
        self.status.setStyleSheet("")
        self.progress.setRange(0, 0)  # Busy until esptool emits a percentage.
        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        env = QProcessEnvironment.systemEnvironment()
        env.insert('PYTHONUNBUFFERED', '1')
        env.insert('NO_COLOR', '1')
        self.process.setProcessEnvironment(env)
        self.process.setProgram(cmd[0])
        self.process.setArguments(cmd[1:])
        self.process.readyReadStandardOutput.connect(self._read_output)
        self.process.errorOccurred.connect(self._process_error)
        self.process.finished.connect(self._finished)
        self.process.start()

    def _read_output(self) -> None:
        if not self.process:
            return
        incoming = bytes(self.process.readAllStandardOutput()).decode('utf-8', errors='replace')
        incoming = re.sub(r'\x1b\[[0-9;?]*[a-zA-Z]', '', incoming)
        self.log = (self.log + incoming)[-80000:]
        self.tail += incoming
        parts = re.split(r'[\r\n]+', self.tail)
        self.tail = parts.pop() if parts else ''
        for line in parts:
            self._parse_line(line.strip())
        if parts:
            self.details.setPlainText(self.log[-15000:])
            scrollbar = self.details.verticalScrollBar()
            scrollbar.setValue(scrollbar.maximum())

    def _parse_line(self, message: str) -> None:
        lower = message.lower()
        if not lower:
            return
        if 'chip is' in lower or 'detected chip' in lower:
            self.status.setText("Cardputer connected. Flashing…")
        elif 'writing' in lower or 'wrote' in lower:
            self.status.setText("Flashing M5 Prom…")
        if 'hash of data verified' in lower or 'data verified' in lower:
            self.verified_count = min(3, self.verified_count + 1)
            self.status.setText(f"Verifying files ({self.verified_count}/3)…")
        if not self.firmware:
            return
        percent = re.search(r'(?<!\d)(\d{1,3}(?:\.\d+)?)\s*%', message)
        if percent and 'writ' in lower:
            p = min(100.0, max(0.0, float(percent.group(1))))
            images = self.firmware.images
            index = min(self.verified_count, 2)
            total = sum(item.length for item in images)
            previous = sum(item.length for item in images[:index])
            progress = int(100 * (previous + images[index].length * p / 100.0) / total)
            self.progress.setRange(0, 100)
            self.progress.setValue(min(99, progress))

    def _process_error(self, _error) -> None:
        if self.busy and self.process and self.process.state() == QProcess.ProcessState.NotRunning:
            self._fail("Couldn't start the flashing tool. Reopen M5 Prom Flasher and try again.")

    def _finished(self, exit_code: int, exit_status) -> None:
        self._read_output()
        if not self.busy:
            return
        if exit_code == 0 and exit_status == QProcess.ExitStatus.NormalExit:
            self._set_busy(False)
            self.progress.setRange(0, 100)
            self.progress.setValue(100)
            self.status.setText("M5 Prom installed successfully. You can disconnect USB.")
            self.status.setStyleSheet("color:#bcebd5;")
            QMessageBox.information(self, "Success", "M5 Prom firmware was flashed successfully!")
        else:
            self._fail("Flashing didn't complete. Check your USB cable, port and Download Mode, then try again.")

    def _fail(self, message: str) -> None:
        if not self.busy:
            return
        self._set_busy(False)
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.status.setText(message)
        self.status.setStyleSheet("color:#ffb0a6;")
        QMessageBox.warning(self, "Flash failed", message + "\n\nShow details for technical information.")

    def closeEvent(self, event) -> None:
        if self.busy:
            QMessageBox.warning(self, "Flashing", "Keep this window open until flashing is complete.")
            event.ignore()
        else:
            event.accept()


def run_gui() -> int:
    app = QApplication.instance() or QApplication(sys.argv)
    app.setApplicationName("M5 Prom Flasher")
    app.setStyle('Fusion')
    window = Flasher()
    window.show()
    return app.exec()

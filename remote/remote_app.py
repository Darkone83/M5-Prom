"""M5 Prom Remote - USB screen mirror, recorder, keyboard and FAT32 manager."""
from __future__ import annotations

import os
import sys
import traceback
import csv
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from native_usb import available_devices
from PySide6.QtCore import Qt, QTimer, Signal, QEvent
from PySide6.QtGui import QColor, QFont, QImage, QKeyEvent, QPixmap
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFileDialog, QFrame, QHBoxLayout, QHeaderView,
    QLabel, QMainWindow, QMessageBox, QPushButton, QProgressBar, QSplitter,
    QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget, QInputDialog,
    QAbstractItemView, QToolButton
)

import protocol as p
from worker import RemoteWorker
from video_recorder import VideoRecorder

STYLESHEET = """
QWidget { background: #12131f; color: #f5f2ff; font: 10pt 'Segoe UI'; }
QMainWindow { background: #12131f; }
QFrame#panel { border: 1px solid #39354c; border-radius: 12px; background: #1b1b2c; }
QFrame#panel QLabel { background: transparent; }
QPushButton, QToolButton { background: #322b45; border: 1px solid #514365;
  border-radius: 8px; padding: 8px 12px; }
QPushButton:hover, QToolButton:hover { background: #46365d; }
QPushButton:pressed { background: #614780; }
QPushButton:disabled { color: #71718a; background: #242331; border-color: #323041; }
QPushButton#primary { background: #9870d7; color: #171126; font-weight: 700; }
QPushButton#primary:hover { background: #b594ed; }
QComboBox { background: #252439; border: 1px solid #514365;
  padding: 8px; border-radius: 8px; }
QComboBox QAbstractItemView { background: #252439; color: white; }
QTableWidget { background: #171723; border: 1px solid #37344a; border-radius: 8px;
  selection-background-color: #574574; gridline-color: #282638; }
QHeaderView::section { background: #30283c; color: #e2d6f8; padding: 7px; border: 0; }
QProgressBar { background: #292637; border: 0; border-radius: 5px; height: 9px; text-align: center; }
QProgressBar::chunk { background: #ac87ef; border-radius: 5px; }
QSplitter::handle { background: #242338; }
"""



class ScreenView(QLabel):
    input_key = Signal(int, int)

    def __init__(self):
        super().__init__("Connect M5 Prom via USB to see its screen")
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(520, 390)
        self.setFrameStyle(QFrame.Shape.StyledPanel)
        self.setStyleSheet("background:#0b0c17; border:1px solid #3d3559; border-radius:10px; color:#9e95b2;")
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.pixmap_source = None

    def set_screen(self, picture: QImage):
        self.pixmap_source = QPixmap.fromImage(picture)
        self._update_image()

    def _update_image(self):
        if self.pixmap_source:
            self.setPixmap(self.pixmap_source.scaled(self.width()-16, self.height()-16,
                Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.FastTransformation))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_image()

    def mousePressEvent(self, event):
        self.setFocus()
        super().mousePressEvent(event)

    def event(self, event):
        # Tab normally changes widget focus; when watching the device it is
        # an M5 Prom editor shortcut and should go to the remote keyboard.
        if event.type() == QEvent.Type.KeyPress and event.key() == Qt.Key.Key_Tab:
            self.input_key.emit(1, ord('\t'))
            return True
        return super().event(event)

    def keyPressEvent(self, event: QKeyEvent):
        special = {
            Qt.Key.Key_Up:2, Qt.Key.Key_Down:3, Qt.Key.Key_Left:4,
            Qt.Key.Key_Right:5, Qt.Key.Key_Return:6, Qt.Key.Key_Enter:6,
            Qt.Key.Key_Backspace:7, Qt.Key.Key_Escape:8,
        }
        if event.key() in special:
            self.input_key.emit(special[event.key()],0)
        elif event.text() and ord(event.text()[0]) < 128 and event.text()[0].isprintable():
            self.input_key.emit(1, ord(event.text()[0]))
        else:
            event.ignore()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("M5 Prom Remote")
        self.setMinimumSize(960, 650)
        self.resize(1190, 750)
        self.worker: RemoteWorker | None = None
        self.current_folder = "/"
        self.entries: list[tuple[str,bool,int]] = []
        self.last_qimage: QImage | None = None
        self.last_pixels = None
        self.writer = None
        self.display_timer = QTimer(self)
        self.display_timer.setInterval(33)
        self.display_timer.timeout.connect(self._display_latest)
        self.record_timer = QTimer(self)
        self.record_timer.timeout.connect(self._record_tick)
        self.is_connected = False
        root = QWidget();self.setCentralWidget(root)
        page = QVBoxLayout(root);page.setContentsMargins(18, 16, 18, 12);page.setSpacing(12)
        top = QHBoxLayout()
        logo = QLabel("M5 <span style='color:#b58bf2'>PROM</span> <span style='font-weight:400;color:#9e9ab6'>REMOTE</span>")
        logo.setTextFormat(Qt.TextFormat.RichText)
        logo.setStyleSheet("font-size:20pt;font-weight:bold")
        top.addWidget(logo);top.addStretch(1)
        top.addWidget(QLabel("USB DEVICE"))
        self.port = QComboBox();self.port.setMinimumWidth(220);top.addWidget(self.port)
        self.rescan = QPushButton("Refresh Devices");self.rescan.clicked.connect(self.find_ports);top.addWidget(self.rescan)
        self.connect_btn = QPushButton("Connect");self.connect_btn.setObjectName("primary")
        self.connect_btn.clicked.connect(self.toggle_connection);top.addWidget(self.connect_btn)
        page.addLayout(top)

        split = QSplitter(Qt.Orientation.Horizontal);split.setChildrenCollapsible(False)
        left = QFrame();left.setObjectName("panel")
        lv = QVBoxLayout(left);lv.setContentsMargins(14,14,14,14);lv.setSpacing(10)
        display_title = QHBoxLayout();display_title.addWidget(QLabel("LIVE DISPLAY  ·  240 × 135"));display_title.addStretch()
        self.live_label = QLabel("OFFLINE");self.live_label.setStyleSheet("color:#aaa3bc")
        display_title.addWidget(self.live_label);lv.addLayout(display_title)
        self.screen = ScreenView();lv.addWidget(self.screen,1)
        self.screen.input_key.connect(self.send_key)
        controls = QHBoxLayout()
        self.capture = QPushButton("Save Screenshot");self.capture.clicked.connect(self.screenshot)
        self.rec = QPushButton("●  Start Recording");self.rec.clicked.connect(self.toggle_recording)
        controls.addWidget(self.capture);controls.addWidget(self.rec);controls.addStretch()
        lv.addLayout(controls)
        hint = QLabel("Click the screen to type. Arrows, Enter, Backspace, Esc, Tab and M5 Prom shortcuts are supported.")
        hint.setWordWrap(True);hint.setStyleSheet("color:#b1adc4;font-size:9pt")

        lv.addWidget(hint)
        self.perf_label = QLabel("Remote diagnostics: waiting for device")
        self.perf_label.setWordWrap(True)
        self.perf_label.setStyleSheet("color:#8fbec4;font-size:9pt")
        lv.addWidget(self.perf_label)
        self.perf_device_label = QLabel("Device: awaiting diagnostic counters")
        self.perf_device_label.setWordWrap(True)
        self.perf_device_label.setStyleSheet("color:#aea7c5;font-size:8pt")
        lv.addWidget(self.perf_device_label)
        self.perf_samples = []
        self.perf_export = QPushButton("Export Diagnostics CSV")
        self.perf_export.setToolTip("Save the measured USB and keyboard counters for analysis")
        self.perf_export.setEnabled(False)
        self.perf_export.clicked.connect(self.export_diagnostics)
        lv.addWidget(self.perf_export)
        split.addWidget(left)

        right = QFrame();right.setObjectName("panel")
        rv = QVBoxLayout(right);rv.setContentsMargins(14,14,14,14);rv.setSpacing(10)
        rv.addWidget(QLabel("MICROSD FILE MANAGER"))
        path_row = QHBoxLayout()
        self.up_btn = QPushButton("Up");self.up_btn.clicked.connect(self.folder_up)
        self.path_text = QLabel("/");self.path_text.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.path_text.setStyleSheet("color:#70dfd8;font-weight:bold")
        self.reload_btn = QPushButton("Refresh");self.reload_btn.clicked.connect(self.refresh_directory)
        path_row.addWidget(self.up_btn);path_row.addWidget(self.path_text,1);path_row.addWidget(self.reload_btn)
        rv.addLayout(path_row)
        self.files = QTableWidget(0,3);self.files.setHorizontalHeaderLabels(["Name","Type","Size"])
        self.files.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.files.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.files.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.files.setShowGrid(False)
        self.files.verticalHeader().setVisible(False)
        self.files.horizontalHeader().setSectionResizeMode(0,QHeaderView.ResizeMode.Stretch)
        self.files.horizontalHeader().setSectionResizeMode(1,QHeaderView.ResizeMode.ResizeToContents)
        self.files.horizontalHeader().setSectionResizeMode(2,QHeaderView.ResizeMode.ResizeToContents)
        self.files.cellDoubleClicked.connect(self.on_doubleclick)
        rv.addWidget(self.files,1)
        row1 = QHBoxLayout()
        self.upload_btn = QPushButton("Upload");self.upload_btn.clicked.connect(self.upload_file)
        self.download_btn = QPushButton("Download");self.download_btn.clicked.connect(self.download_file)
        self.mkdir_btn = QPushButton("New Folder");self.mkdir_btn.clicked.connect(self.make_folder)
        row1.addWidget(self.upload_btn);row1.addWidget(self.download_btn);row1.addWidget(self.mkdir_btn)
        rv.addLayout(row1)
        row2 = QHBoxLayout()
        self.rename_btn=QPushButton("Rename");self.rename_btn.clicked.connect(self.rename_file)
        self.delete_btn=QPushButton("Delete");self.delete_btn.clicked.connect(self.delete_file)
        row2.addWidget(self.rename_btn);row2.addWidget(self.delete_btn);row2.addStretch()
        rv.addLayout(row2)
        transfer_row = QHBoxLayout()
        self.progress = QProgressBar();self.progress.setRange(0,100);self.progress.setValue(0)
        self.progress.setFormat("Ready")
        transfer_row.addWidget(self.progress)
        rv.addLayout(transfer_row)
        note=QLabel("Files are transferred through the Cardputer's mounted FAT32 filesystem. EEPROM maintenance takes priority.")
        note.setWordWrap(True);note.setStyleSheet("color:#aba6c0;font-size:9pt")
        rv.addWidget(note)
        split.addWidget(right);split.setSizes([660,490]);page.addWidget(split,1)
        self.status = QLabel("Connect the Cardputer with a USB data cable, then click Connect.")
        self.status.setStyleSheet("color:#b5b1c7")
        page.addWidget(self.status)
        self.file_controls=[self.upload_btn,self.download_btn,self.mkdir_btn,self.rename_btn,self.delete_btn,
                            self.reload_btn,self.up_btn]
        self._set_controls(False)
        self.find_ports()

    def find_ports(self):
        current = self.port.currentData()
        self.port.clear()
        try:
            available = available_devices()
        except (OSError, RuntimeError) as exc:
            available = []
            self.status.setText(str(exc))
        for device in available:
            self.port.addItem(device.label, device.identifier)
        if current:
            idx = self.port.findData(current)
            if idx >= 0: self.port.setCurrentIndex(idx)
        if not available:
            self.port.addItem("No M5 Prom USB device found", None)

    def _set_controls(self,enabled:bool):
        for w in self.file_controls:w.setEnabled(enabled)
        self.capture.setEnabled(enabled and self.last_qimage is not None)
        self.rec.setEnabled(enabled and self.last_qimage is not None)

    def toggle_connection(self):
        if self.worker is not None:
            self.disconnect_requested()
            return
        device_id = self.port.currentData()
        if not device_id:
            QMessageBox.information(self,"USB device not found","Connect M5 Prom with a USB data cable, then click Refresh Devices.")
            return
        self.status.setText("Connecting to M5 Prom...")
        self.worker=RemoteWorker(device_id)
        self.worker.connected.connect(self._connected)
        self.worker.disconnected.connect(self._disconnected)
        # Do not queue every frame in Qt. Show only the latest frame each tick.
        self.display_timer.start()
        self.worker.entry.connect(self._entry)
        self.worker.file_done.connect(self._file_done)
        self.worker.progress.connect(self._transfer_progress)
        self.worker.info.connect(self.status.setText)
        self.worker.performance.connect(self._performance)
        self.worker.start()
        self.connect_btn.setText("Disconnect");self.port.setEnabled(False);self.rescan.setEnabled(False)

    def _connected(self,message:str):
        self.is_connected=True
        self.status.setText(message)
        self.live_label.setText("● PC MODE")
        self.live_label.setStyleSheet("color:#68e0b5;font-weight:bold")
        self._set_controls(True)
        self.screen.setFocus()
        self.refresh_directory()

    def disconnect_requested(self):
        if self.worker:self.worker.shutdown()
        self.connect_btn.setEnabled(False)

    def _disconnected(self,message:str):
        self.stop_recording()
        self.display_timer.stop()
        self.is_connected=False
        self._set_controls(False)
        self.live_label.setText("OFFLINE");self.live_label.setStyleSheet("color:#aba5bc")

        self.status.setText(message)
        self.perf_label.setText("Remote diagnostics: disconnected")
        self.perf_device_label.setText("Device: disconnected")
        self.connect_btn.setEnabled(True);self.connect_btn.setText("Connect")
        self.port.setEnabled(True);self.rescan.setEnabled(True)
        # Worker's QThread emits this after exiting run(); wait for clean end.
        if self.worker:
            self.worker.wait(2000)
            self.worker.deleteLater()
            self.worker=None

    def _display_latest(self):
        if self.worker is not None:
            raw = self.worker.take_latest_frame()
            if raw is not None:
                self._frame(raw)

    def _frame(self,raw:bytes):
        if len(raw)!=p.ROW_BYTES*p.HEIGHT:return
        pixels=np.frombuffer(raw,dtype=">u2").reshape((p.HEIGHT,p.WIDTH))
        rgb=np.empty((p.HEIGHT,p.WIDTH,3),dtype=np.uint8)
        rgb[:,:,0]=((pixels>>11)&31)*255//31
        rgb[:,:,1]=((pixels>>5)&63)*255//63
        rgb[:,:,2]=(pixels&31)*255//31
        self.last_pixels=rgb
        self.last_qimage=QImage(rgb.data,p.WIDTH,p.HEIGHT,p.WIDTH*3,QImage.Format.Format_RGB888).copy()
        self.screen.set_screen(self.last_qimage)
        self._set_controls(self.is_connected)


    def _performance(self, data: dict):
        device = data['device']
        self.perf_samples.append({
            'utc_time': datetime.now(timezone.utc).isoformat(timespec='seconds'),
            'fps': data['fps'],
            'usb_rx_kbs': data['rx_kbs'],
            'device_usb_tx_kbs': data['device_tx_kbs'],
            'key_ack_p95_ms': data['key_render_p95_ms'],
            'device_ui_p95_ms': data['device_key_p95_ms'],
            'device_key_drops': device.keys_dropped if device else None,
            'device_key_queue_peak': device.key_queue_peak if device else None,
            'device_diagnostic_ack_drops': device.diagnostic_ack_drops if device else None,
            'device_screen_backpressure': device.screen_backpressure if device else None,
            'device_frame_count': device.frames_queued if device else None,
            'device_heap_free': device.heap_free if device else None,
            'device_heap_min': device.heap_low if device else None,
            'device_remote_stack_free': device.stack_free if device else None,
            'device_tick_ms': device.tick_ms if device else None,
        })
        if len(self.perf_samples) > 3600:
            self.perf_samples.pop(0)
        self.perf_export.setEnabled(True)
        def latency(value):
            return "—" if value is None else f"{value:.0f} ms"
        self.perf_label.setText(
            f"Screen {data['fps']:.1f} FPS · USB IN {data['rx_kbs']:.1f} KB/s · "
            f"Key handled ACK p95 {latency(data['key_render_p95_ms'])} "
            f"(device queue+UI p95 {latency(data['device_key_p95_ms'])})"
        )
        d = data['device']
        if d is not None:
            tx_rate = data['device_tx_kbs']
            tx_text = f"{tx_rate:.1f}" if tx_rate is not None else "—"
            self.perf_device_label.setText(
                f"Device USB IN→PC {tx_text} KB/s · "
                f"Keys queued/handled {d.keys_queued}/{d.keys_rendered} · "
                f"Key drops {d.keys_dropped} · ACK drops {d.diagnostic_ack_drops} · "
                f"Peak Q {d.key_queue_peak} · "
                f"TX backpressure {d.screen_backpressure} · Tick {d.tick_ms} ms · "
                f"Heap {d.heap_free // 1024} KB"
            )

    def export_diagnostics(self):
        if not self.perf_samples:
            QMessageBox.information(self, "Diagnostics", "Connect and collect some readings first.")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Export M5 Prom performance readings", "M5Prom-Remote-Diagnostics.csv",
            "CSV Files (*.csv)")
        if not path:
            return
        try:
            with open(path, "w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=list(self.perf_samples[0]))
                writer.writeheader()
                writer.writerows(self.perf_samples)
            self.status.setText(f"Performance readings exported: {path}")
        except OSError as exc:
            QMessageBox.warning(self, "Export failed", str(exc))

    def send_key(self,which:int,char:int):
        if self.worker and self.is_connected:
            self.worker.send(p.KEY,bytes((which,char&255)))

    def screenshot(self):
        if self.last_qimage is None:return
        path,_=QFileDialog.getSaveFileName(self,"Save M5 Prom screenshot","M5Prom-Screenshot.png","PNG Image (*.png)")
        if path:
            if not self.last_qimage.save(path,"PNG"):
                QMessageBox.warning(self,"Screenshot failed","Could not save that PNG file.")
            else:self.status.setText(f"Screenshot saved: {path}")

    def toggle_recording(self):
        if self.writer:
            self.stop_recording();return
        if self.last_pixels is None:return
        path,_=QFileDialog.getSaveFileName(self,"Record M5 Prom display","M5Prom-Recording.mp4","MP4 Video (*.mp4)")
        if not path:return
        if not path.lower().endswith(".mp4"):path += ".mp4"
        try:
            import imageio.v2 as imageio
            writer=imageio.get_writer(path,format="FFMPEG",fps=12,codec="libx264",quality=7,macro_block_size=1)
            self.writer=VideoRecorder(writer)
            self.writer.start()
            self.record_timer.start(1000//12)
            self.rec.setText("■  Stop Recording")
            self.status.setText(f"Recording MP4 to: {path}")
        except Exception as exc:
            self.writer=None
            QMessageBox.warning(self,"Recording unavailable",f"Could not start MP4 recording:\n{exc}")

    def _record_tick(self):
        if self.writer:
            if self.writer.error:
                reason = self.writer.error
                self.stop_recording()
                QMessageBox.warning(self, "Recording stopped", reason)
            elif self.last_pixels is not None:
                self.writer.offer(self.last_pixels)

    def stop_recording(self):
        self.record_timer.stop()
        if self.writer:
            # MP4 finalization happens on the encoder thread, never in Qt UI.
            self.writer.finish()
            self.writer = None
        self.rec.setText("●  Start Recording")

    def _selected(self):
        idx=self.files.currentRow()
        if idx<0:return None
        return self.entries[idx] if idx<len(self.entries) else None

    def _child(self,name:str)->str:
        return p.sd_child(self.current_folder,name)

    def refresh_directory(self):
        if self.worker and self.is_connected:
            self.entries=[];self.files.setRowCount(0)
            self.worker.list_directory(self.current_folder)
            self.status.setText("Loading SD directory...")

    def folder_up(self):
        if self.current_folder=="/":return
        self.current_folder=self.current_folder.rsplit("/",1)[0] or "/"
        self.path_text.setText(self.current_folder)
        self.refresh_directory()

    def on_doubleclick(self,row:int,column:int):
        if row>=len(self.entries):return
        name,is_dir,_=self.entries[row]
        if is_dir:
            self.current_folder=self._child(name)
            self.path_text.setText(self.current_folder)
            self.refresh_directory()
        else:self.download_file()

    def _entry(self,name:str,is_dir:bool,size:int):
        self.entries.append((name,is_dir,size))
        index=len(self.entries)-1
        self.files.insertRow(index)
        self.files.setItem(index,0,QTableWidgetItem(("📁 " if is_dir else "📄 ")+name))
        self.files.setItem(index,1,QTableWidgetItem("Folder" if is_dir else "File"))
        self.files.setItem(index,2,QTableWidgetItem("" if is_dir else self._format_size(size)))

    @staticmethod
    def _format_size(n:int):
        if n<1024:return f"{n} B"
        if n<1024*1024:return f"{n/1024:.1f} KB"
        return f"{n/(1024*1024):.1f} MB"

    def _file_done(self,ok:bool,message:str):
        self.status.setText(("✓ " if ok else "✕ ")+message)
        self.progress.setFormat("Ready") if ok else self.progress.setFormat("Failed")
        if ok and self.worker and self.is_connected:
            # Only refresh after mutation (not a directory listing or download).
            if message == "SD operation complete" or message == "Upload complete":
                QTimer.singleShot(100,self.refresh_directory)
        if not ok and self.is_connected:
            QMessageBox.warning(self,"SD operation failed",message)

    def _transfer_progress(self,current:int,total:int):
        self.progress.setValue(min(100,round(current*100/max(1,total))))
        self.progress.setFormat(f"{self._format_size(current)} / {self._format_size(total)}")

    def download_file(self):
        chosen=self._selected()
        if not chosen or chosen[1]:return
        name=chosen[0]
        path,_=QFileDialog.getSaveFileName(self,"Download from M5 Prom",name,"All files (*)")
        if path and self.worker:
            self.worker.download(self._child(name),path)

    def upload_file(self):
        source,_=QFileDialog.getOpenFileName(self,"Select a file to upload","","All files (*)")
        if source and self.worker:
            try:self.worker.upload(source,self._child(Path(source).name))
            except ValueError as exc:QMessageBox.warning(self,"Invalid filename",str(exc))

    def make_folder(self):
        name,ok=QInputDialog.getText(self,"New SD folder","Folder name")
        if ok and name and self.worker:
            try:self.worker.mutate(p.FS_MKDIR,self._child(name))
            except ValueError as exc:QMessageBox.warning(self,"Invalid folder name",str(exc))

    def rename_file(self):
        entry=self._selected()
        if not entry:return
        name,ok=QInputDialog.getText(self,"Rename SD item","New name",text=entry[0])
        if ok and name and name!=entry[0] and self.worker:
            try:self.worker.mutate(p.FS_RENAME,self._child(entry[0]),self._child(name))
            except ValueError as exc:QMessageBox.warning(self,"Invalid filename",str(exc))

    def delete_file(self):
        entry=self._selected()
        if not entry:return
        if QMessageBox.question(self,"Delete from SD",f"Delete '{entry[0]}' from M5 Prom?\n\nThis cannot be undone. Non-empty folders cannot be deleted.",
            QMessageBox.StandardButton.Yes|QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No)==QMessageBox.StandardButton.Yes:
            if self.worker:self.worker.mutate(p.FS_DELETE,self._child(entry[0]))

    def closeEvent(self,event):
        self.stop_recording()
        if self.worker:
            self.worker.shutdown()
            self.worker.wait(2500)
        super().closeEvent(event)


def main():
    app=QApplication(sys.argv)
    app.setApplicationName("M5 Prom Remote")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)
    win=MainWindow();win.show()
    return app.exec()


if __name__ == "__main__":
    try:sys.exit(main())
    except Exception:
        path=Path(__file__).resolve().parent/"remote-error.log"
        path.write_text(traceback.format_exc(),encoding="utf-8")
        print(f"M5 Prom Remote failed; details saved to {path}",file=sys.stderr)
        raise

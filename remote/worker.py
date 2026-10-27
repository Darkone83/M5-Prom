"""Native USB bulk I/O and SD transfers. Runs in a QThread; never blocks the GUI."""
from __future__ import annotations

import os
import queue
import struct
import tempfile
import threading
import time
from pathlib import Path

from native_usb import NativeUSB
from PySide6.QtCore import QThread, Signal

import protocol as p
from perf_metrics import PerformanceMonitor


class RemoteWorker(QThread):
    connected = Signal(str)
    disconnected = Signal(str)
    frame = Signal(bytes)
    entry = Signal(str, bool, int)
    file_done = Signal(bool, str)
    progress = Signal(int, int)
    info = Signal(str)
    performance = Signal(dict)

    def __init__(self, device_id: str):
        super().__init__()
        self.device_id = device_id
        self.queue = queue.Queue()
        self.stopping = False
        self.usb = None
        # Native USB bulk IN/OUT can run concurrently. A slow bulk OUT write
        # must never stop the bulk IN reader from draining screen packets.
        self._tx_queue: queue.Queue[bytes | None] | None = None
        self._tx_thread: threading.Thread | None = None
        self._tx_error: str | None = None
        self.decoder = p.Decoder()
        self.active = False
        self.last_seen = 0.0
        self.last_ping = 0.0
        self.last_hello = 0.0
        self.frame_id = None
        self.frame_dirty = False
        self.fast_frames = False
        self.perf_capability = False
        self.perf = PerformanceMonitor()
        self.key_sequence = 0
        self.last_perf_request = 0.0
        self.last_perf_emit = 0.0
        self.screen = bytearray(p.WIDTH * p.HEIGHT * 2)
        # Latest-only mailbox: GUI polls at 30 FPS instead of accumulating
        # potentially hundreds of queued Qt frame deliveries during a stall.
        self._frame_lock = threading.Lock()
        self._latest_frame: bytes | None = None
        self.operation = ""
        self.download_target: Path | None = None
        self.download_temp: Path | None = None
        self.download_file = None
        self.upload_file = None
        self.upload_size = 0
        self.transferred = 0

    def take_latest_frame(self) -> bytes | None:
        with self._frame_lock:
            result = self._latest_frame
            self._latest_frame = None
            return result

    def send(self, kind: int, data: bytes = b"") -> None:
        if kind == p.KEY and self.active and self.perf_capability and len(data) == 2:
            # Optional v1 extension; older firmware still receives 2-byte keys.
            sequence = self.key_sequence
            self.key_sequence = (self.key_sequence + 1) % 65535
            self.perf.on_key(sequence)
            data += struct.pack("<H", sequence)
        self.queue.put(("packet", kind, data))

    def list_directory(self, path: str) -> None:
        self.queue.put(("list", path))

    def download(self, remote_path: str, local_file: str) -> None:
        self.queue.put(("download", remote_path, local_file))

    def upload(self, local_file: str, remote_path: str) -> None:
        self.queue.put(("upload", local_file, remote_path))

    def mutate(self, command: int, first: str, second: str = "") -> None:
        self.queue.put(("mutate", command, first, second))

    def shutdown(self) -> None:
        self.stopping = True

    def _send(self, kind: int, data: bytes = b"") -> None:
        assert self.usb is not None
        packet = p.pack(kind, data)
        # In the GUI's running worker, enqueue instead of blocking the RX loop
        # for a USB write. Direct/test usage remains synchronous.
        if self._tx_queue is None:
            self.usb.write(packet)
        else:
            if self._tx_error is not None:
                raise OSError(self._tx_error)
            try:
                self._tx_queue.put_nowait(packet)
            except queue.Full as exc:
                raise OSError("USB commands are backed up; reconnect the device") from exc

    def _usb_tx_loop(self) -> None:
        assert self._tx_queue is not None
        while True:
            packet = self._tx_queue.get()
            try:
                if packet is None:
                    break
                assert self.usb is not None
                self.usb.write(packet)
            except (OSError, ValueError) as exc:
                self._tx_error = f"USB OUT transfer stalled: {exc}"
                break
            finally:
                self._tx_queue.task_done()

    def _start_usb_tx(self) -> None:
        self._tx_error = None
        self._tx_queue = queue.Queue(maxsize=96)
        self._tx_thread = threading.Thread(target=self._usb_tx_loop,
                                          name="M5Prom-USB-Out", daemon=True)
        self._tx_thread.start()

    def _stop_usb_tx(self) -> None:
        tx, thread = self._tx_queue, self._tx_thread
        if tx is not None and thread is not None:
            try:
                tx.put(None, timeout=0.1)
            except queue.Full:
                pass
            # No GUI lock is held. If a USB write has timed out, this writer
            # exits independently without holding the screen receive path.
            thread.join(timeout=0.5)
        self._tx_queue = None
        self._tx_thread = None

    def _cleanup_files(self) -> None:
        if self.download_file is not None:
            self.download_file.close()
            self.download_file = None
        if self.download_temp is not None:
            try: self.download_temp.unlink(missing_ok=True)
            except OSError: pass
            self.download_temp = None
        if self.upload_file is not None:
            self.upload_file.close()
            self.upload_file = None
        self.download_target = None
        self.upload_size = self.transferred = 0

    def _finish(self, ok: bool, message: str) -> None:
        if self.operation == "download" and ok:
            if self.download_file is None or self.download_temp is None or self.download_target is None:
                ok, message = False, "Download did not start"
            elif self.transferred != self.upload_size:
                ok, message = False, "Download incomplete; partial file discarded"
            else:
                self.download_file.flush()
                os.fsync(self.download_file.fileno())
                self.download_file.close()
                self.download_file = None
                os.replace(self.download_temp, self.download_target)
                self.download_temp = None
                message = "File downloaded and saved"
        self._cleanup_files()
        self.operation = ""
        self.file_done.emit(ok, message)

    def _upload_next(self) -> None:
        if self.upload_file is None: return
        chunk = self.upload_file.read(512)
        if chunk:
            self._send(p.FS_PUT_CHUNK, chunk)
        else:
            self._send(p.FS_PUT_DONE)

    def _command(self, job) -> None:
        what = job[0]
        if what == "packet":
            self._send(job[1], job[2]); return
        if not self.active or self.operation:
            self.info.emit("Connect first or finish the current file operation"); return
        if what == "list":
            self.operation = "list"
            self._send(p.FS_LIST, job[1].encode("utf-8"))
        elif what == "download":
            self.operation = "download"
            self.download_target = Path(job[2])
            self._send(p.FS_GET, job[1].encode("utf-8"))
        elif what == "upload":
            self.operation = "upload"
            self.upload_file = open(job[1], "rb")
            self.upload_size = os.path.getsize(job[1])
            if self.upload_size > 16 * 1024 * 1024:
                self._finish(False, "File exceeds the 16 MB USB transfer limit")
                return
            payload = struct.pack("<I", self.upload_size) + job[2].encode("utf-8")
            self._send(p.FS_PUT_BEGIN, payload)
        elif what == "mutate":
            self.operation = "change"
            payload = job[2].encode("utf-8")
            if job[1] == p.FS_RENAME:
                payload += b"\0" + job[3].encode("utf-8")
            self._send(job[1], payload)

    def _on_packet(self, item: p.Packet) -> None:
        kind, buf = item.kind, item.payload
        if kind == p.HELLO_ACK:
            if len(buf) < 6 or buf[0] != p.VERSION or struct.unpack_from("<HH", buf, 1) != (p.WIDTH, p.HEIGHT):
                self.info.emit("Unsupported M5 Prom remote protocol or display size"); return
            self.last_seen = time.monotonic()
            if not self.active:
                self.active = True
                self.fast_frames = bool(buf[5] & p.CAP_RLE)
                self.perf_capability = bool(buf[5] & p.CAP_PERF)
                self.connected.emit("M5 Prom connected · PC Mode active" +
                                    (" · Fast Screen" if self.fast_frames else ""))
                # Optional extension: older firmware ignores this capability,
                # so use legacy uncompressed rows unless it is advertised.
                if self.fast_frames:
                    self._send(p.FAST, b"\x01")
                self._send(p.FULL)
            return
        if not self.active: return
        if kind == p.PONG:
            self.last_seen = time.monotonic()
        elif kind == p.KEY_STATUS:
            self.perf.on_key_status(buf)
        elif kind == p.PERF_REPLY:
            self.perf.on_device_stats(buf)
        elif kind == p.FRAME_START and len(buf) == 2:
            self.frame_id = struct.unpack("<H", buf)[0]
            self.frame_dirty = False
        elif kind in (p.FRAME_ROW, p.FRAME_RLE) and len(buf) >= 3:
            frame_id, row = struct.unpack_from("<HB", buf)
            if frame_id == self.frame_id and row < p.HEIGHT:
                if kind == p.FRAME_RLE:
                    try:
                        pixels = p.unpack_rle_row(buf[3:])
                    except ValueError:
                        # A damaged optional packet must not produce a partial
                        # screen; request a complete refresh instead.
                        self.frame_id = None
                        self.frame_dirty = False
                        self._send(p.FULL)
                        return
                elif len(buf) == p.ROW_BYTES + 3:
                    pixels = buf[3:]
                else:
                    return
                pos = row * p.ROW_BYTES
                self.screen[pos:pos+p.ROW_BYTES] = pixels
                self.frame_dirty = True
        elif kind == p.FRAME_END and len(buf) == 2:
            if self.frame_id == struct.unpack("<H", buf)[0]:
                if self.frame_dirty:
                    rendered = bytes(self.screen)
                    with self._frame_lock:
                        self._latest_frame = rendered
                    # Keep this signal available for existing protocol clients
                    # and regression tests. The GUI uses the mailbox instead.
                    self.frame.emit(rendered)
                    self.perf.on_frame()
                self.frame_id = None
                self.frame_dirty = False
        elif kind == p.FS_ENTRY and len(buf) >= 6:
            is_dir, size = struct.unpack_from("<BI", buf)
            name = buf[5:].decode("utf-8", "replace")
            self.entry.emit(name, bool(is_dir), size)
        elif kind == p.FS_GET_BEGIN and self.operation == "download" and len(buf) == 4:
            self.upload_size = struct.unpack("<I", buf)[0]
            assert self.download_target is not None
            # Do not confuse interrupted downloads with complete files.
            # Unique temporary file: avoids clobbering stale/other downloads.
            fd, temp_name = tempfile.mkstemp(prefix=self.download_target.name+".m5prom-",
                                            suffix=".part",dir=self.download_target.parent)
            self.download_temp = Path(temp_name)
            self.download_file = os.fdopen(fd, "wb")
            self.transferred = 0
            self.progress.emit(0, self.upload_size)
        elif kind == p.FS_DATA and self.operation == "download":
            if self.download_file is None:
                self._send(p.FS_CANCEL);return
            self.download_file.write(buf)
            self.transferred += len(buf)
            self.progress.emit(self.transferred, self.upload_size)
        elif kind == p.FS_READY and self.operation == "upload":
            if len(buf) and buf[0] == 0:
                self._upload_next()
        elif kind == p.FS_ACK and self.operation == "upload" and len(buf) == 4:
            count = struct.unpack("<I", buf)[0]
            if count < self.transferred or count > self.upload_size:
                self._send(p.FS_CANCEL)
                self._finish(False, "USB upload acknowledgement out of range")
                return
            self.transferred = count
            self.progress.emit(count, self.upload_size)
            self._upload_next()
        elif kind == p.FS_DONE and len(buf):
            code = buf[0]
            detail = buf[1:].split(b"\0", 1)[0].decode("utf-8", "replace")
            self._finish(code == 0, detail if code == 0 else (detail or p.STATUS.get(code, "SD operation failed")))

    def _drain_commands(self, limit: int = 12) -> None:
        for _ in range(limit):
            try:
                job = self.queue.get_nowait()
            except queue.Empty:
                break
            try:
                self._command(job)
            except (OSError, ValueError) as exc:
                if self.operation:
                    self._finish(False, str(exc))
                else:
                    self.info.emit(str(exc))

    def run(self) -> None:
        reason = "Connection closed"
        try:
            self.usb = NativeUSB(self.device_id)
            self._start_usb_tx()
            self.last_hello = self.last_ping = 0.0
            while not self.stopping:
                if self._tx_error is not None:
                    reason = self._tx_error
                    break
                now = time.monotonic()
                if not self.active and now-self.last_hello > 1.0:
                    self._send(p.HELLO, bytes([p.VERSION]));self.last_hello = now
                if self.active and now-self.last_ping > 1.0:
                    self._send(p.PING);self.last_ping = now
                if self.active and self.perf_capability and now-self.last_perf_request > 1.0:
                    self._send(p.PERF_REQUEST);self.last_perf_request = now
                # Send PC keys before reading the next batch of screen data.
                self._drain_commands()
                data = self.usb.read(8192)
                self.perf.on_rx(len(data))
                for index, item in enumerate(self.decoder.feed(data)):
                    if index and index % 12 == 0:
                        self._drain_commands(6)
                    try:
                        self._on_packet(item)
                    except (OSError, ValueError) as exc:
                        # A local file permission/path failure should not
                        # destroy the USB session or the live screen.
                        if self.operation:
                            self._send(p.FS_CANCEL)
                            self._finish(False, str(exc))
                        else:
                            self.info.emit(str(exc))
                self._drain_commands()
                if self.active and now-self.last_perf_emit > 1.0:
                    self.performance.emit(self.perf.snapshot())
                    self.last_perf_emit = now
                if self.active and now-self.last_seen > 6.0:
                    reason="Cardputer heartbeat timed out"
                    break
            if self.active and self.usb and self.usb.is_open:
                try: self._send(p.QUIT)
                except OSError: pass
        except (OSError,ValueError) as exc:
            reason = str(exc)
        finally:
            self._cleanup_files()
            self._stop_usb_tx()
            if self.usb is not None:
                try:self.usb.close()
                except OSError: pass
            self.active = False
            self.disconnected.emit(reason)

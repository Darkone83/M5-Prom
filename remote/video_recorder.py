"""Bounded asynchronous MP4 encoding to keep M5 Prom controls responsive."""
from __future__ import annotations
import threading
import queue
import numpy as np

class VideoRecorder(threading.Thread):
    """Encode frames off the Qt UI thread with a bounded latest-frame queue.

    Recording never holds keyboard or screen painting hostage when ffmpeg is
    temporarily slow. When overloaded it skips older frames rather than using
    unbounded PC memory; the output still uses the configured 12 FPS timebase.
    """
    def __init__(self, writer):
        super().__init__(name="m5prom-video-encoder", daemon=False)
        self.writer = writer
        self.pending = queue.Queue(maxsize=2)
        self.finishing = threading.Event()
        self.error: str | None = None

    def offer(self, rgb) -> None:
        if self.finishing.is_set():
            return
        try:
            self.pending.put_nowait(rgb)
        except queue.Full:
            try:
                self.pending.get_nowait()  # drop outdated frame, keep newest
            except queue.Empty:
                pass
            try:
                self.pending.put_nowait(rgb)
            except queue.Full:
                pass

    def finish(self) -> None:
        self.finishing.set()

    def run(self) -> None:
        try:
            while not self.finishing.is_set() or not self.pending.empty():
                try:
                    rgb = self.pending.get(timeout=0.05)
                except queue.Empty:
                    continue
                self.writer.append_data(np.repeat(np.repeat(rgb, 4, axis=0), 4, axis=1))
        except Exception as exc:
            self.error = str(exc)
        finally:
            try:
                self.writer.close()
            except Exception as exc:
                self.error = str(exc)



"""Low-overhead Pass 1 diagnostics for M5 Prom Remote.

All host intervals use one monotonic clock. Device queue processing time is
reported separately; cross-device absolute timestamps are never compared.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import struct
import threading
import time

import protocol as p


@dataclass(frozen=True)
class DeviceStats:
    tick_ms: int
    uptime_ms: int
    rx_bytes: int
    tx_bytes: int
    frames_queued: int
    keys_queued: int
    keys_rendered: int
    keys_dropped: int
    screen_backpressure: int
    publish_lock_misses: int
    diagnostic_ack_drops: int
    heap_free: int
    heap_low: int
    key_queue_peak: int
    stack_free: int

    @classmethod
    def decode(cls, packet: bytes) -> "DeviceStats":
        if len(packet) != p.PERF_SIZE:
            raise ValueError("Invalid diagnostic packet size")
        fields = struct.unpack(p.PERF_FORMAT, packet)
        if fields[0] != 1:
            raise ValueError("Unsupported diagnostic layout")
        return cls(fields[2], *fields[3:])


def percentile(values: deque[float], percent: float) -> float | None:
    if not values:
        return None
    data = sorted(values)
    index = (len(data) - 1) * percent
    low = int(index)
    return data[low] + (data[min(low + 1, len(data) - 1)] - data[low]) * (index - low)


class PerformanceMonitor:
    """One-second transport rates and trailing key-ACK latencies."""
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.lock = threading.Lock()
        self.key_sent: dict[int, float] = {}
        self.receipt_latency = deque(maxlen=120)
        self.rendered_latency = deque(maxlen=120)
        self.device_render_ms = deque(maxlen=120)
        self.key_drop_acks = 0
        self.total_rx = 0
        self.completed_frames = 0
        self.period_start = self.clock()
        self.period_rx = 0
        self.period_frames = 0
        self.device: DeviceStats | None = None
        self._device_prev: DeviceStats | None = None
        self._device_prev_time: float | None = None
        self._device_tx_rate = None

    def on_key(self, seq: int) -> None:
        with self.lock:
            self.key_sent[seq] = self.clock()
            # Bounded even if a disconnected device never acknowledges input.
            while len(self.key_sent) > 256:
                self.key_sent.pop(next(iter(self.key_sent)))

    def on_key_status(self, payload: bytes) -> None:
        if len(payload) != 5:
            return
        seq, stage, device_ms = struct.unpack("<HBH", payload)
        with self.lock:
            start = self.key_sent.get(seq)
            if start is None:
                return
            host_ms = max(0.0, (self.clock() - start) * 1000.0)
            if stage == p.KEY_QUEUED:
                self.receipt_latency.append(host_ms)
            elif stage == p.KEY_RENDERED:
                self.rendered_latency.append(host_ms)
                self.device_render_ms.append(float(device_ms))
                self.key_sent.pop(seq, None)
            elif stage == p.KEY_DROPPED:
                self.key_drop_acks += 1
                self.key_sent.pop(seq, None)

    def on_rx(self, size: int) -> None:
        if size > 0:
            with self.lock:
                self.total_rx += size
                self.period_rx += size

    def on_frame(self) -> None:
        with self.lock:
            self.completed_frames += 1
            self.period_frames += 1

    def on_device_stats(self, payload: bytes) -> None:
        try:
            sample = DeviceStats.decode(payload)
        except ValueError:
            return
        now = self.clock()
        with self.lock:
            if self._device_prev is not None and self._device_prev_time is not None:
                interval = now - self._device_prev_time
                diff = (sample.tx_bytes - self._device_prev.tx_bytes) & 0xFFFFFFFF
                self._device_tx_rate = (diff / interval / 1024.0) if interval > 0 else None
            self._device_prev = sample
            self._device_prev_time = now
            self.device = sample

    def snapshot(self) -> dict:
        now = self.clock()
        with self.lock:
            delta = max(0.001, now - self.period_start)
            result = {
                "fps": round(self.period_frames / delta, 1),
                "rx_kbs": round(self.period_rx / delta / 1024.0, 1),
                "device_tx_kbs": None if self._device_tx_rate is None else round(self._device_tx_rate, 1),
                "key_recv_p95_ms": percentile(self.receipt_latency, .95),
                "key_render_p95_ms": percentile(self.rendered_latency, .95),
                "device_key_p95_ms": percentile(self.device_render_ms, .95),
                "key_drop_acks": self.key_drop_acks,
                "pending_keys": len(self.key_sent),
                "device": self.device,
            }
            self.period_rx = 0
            self.period_frames = 0
            self.period_start = now
            return result

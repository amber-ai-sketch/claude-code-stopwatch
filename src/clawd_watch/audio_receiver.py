"""Audio frame protocol + receiver for clawd-watch push-to-talk.

The watch streams microphone PCM to the daemon over a DEDICATED BLE audio
characteristic (separate 128-bit service, NOT the newline-JSON NUS channel).
Audio bytes routinely contain 0x0A, so they can never share the line-delimited
control channel.

Wire frame (little-endian), shared contract with firmware audio_frame.h:

    ┌────────┬──────────┬─────────────┬───────────────────────────┐
    │ type   │ seq      │ timestamp   │ payload                   │
    │ 1 byte │ 2 bytes  │ 4 bytes     │ variable                  │
    └────────┴──────────┴─────────────┴───────────────────────────┘
      type:      0x01 STREAM_START, 0x02 AUDIO_CHUNK, 0x03 STREAM_END
      seq:       uint16 LE, per-stream frame counter (start=0, chunks=1..N,
                 end=N+1). daemon uses it to detect gaps / reorder / dedup.
      timestamp: uint32 LE, firmware SAMPLING-instant ms (esp_timer). Phase 1
                 sole use = detect sampling-timeline holes (chunked blocking
                 record() can drop audio between main-loop ticks). NOT for STT.
      payload:   STREAM_START → uint32 LE sample_rate (daemon must not assume
                                8kHz: firmware may fall back to 44.1k)
                 AUDIO_CHUNK  → raw signed 16-bit PCM, little-endian
                 STREAM_END   → uint16 LE total_chunks (lets daemon confirm it
                                received every chunk)

This module is the Phase-2-reusable receive core: parse_frame() and
reassemble() are pure functions with no BLE / disk coupling.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

# Frame type bytes.
TYPE_STREAM_START = 0x01
TYPE_AUDIO_CHUNK = 0x02
TYPE_STREAM_END = 0x03

HEADER_LEN = 7  # type(1) + seq(2) + timestamp(4)

# Bytes per PCM sample (signed 16-bit mono).
BYTES_PER_SAMPLE = 2


class AudioFrameError(ValueError):
    """A received audio frame violated the wire format. Carries the offending
    byte length so logs say WHAT was wrong, not just THAT it was wrong."""


@dataclass(frozen=True)
class AudioFrame:
    type: int
    seq: int
    timestamp_ms: int
    payload: bytes

    @property
    def sample_rate(self) -> int:
        """Only valid on STREAM_START frames."""
        if self.type != TYPE_STREAM_START:
            raise AudioFrameError(
                f"sample_rate read on non-start frame type={self.type:#04x}"
            )
        if len(self.payload) != 4:
            raise AudioFrameError(
                f"STREAM_START payload must be 4 bytes, got {len(self.payload)}"
            )
        return struct.unpack_from("<I", self.payload)[0]

    @property
    def total_chunks(self) -> int:
        """Only valid on STREAM_END frames."""
        if self.type != TYPE_STREAM_END:
            raise AudioFrameError(
                f"total_chunks read on non-end frame type={self.type:#04x}"
            )
        if len(self.payload) != 2:
            raise AudioFrameError(
                f"STREAM_END payload must be 2 bytes, got {len(self.payload)}"
            )
        return struct.unpack_from("<H", self.payload)[0]


def parse_frame(data: bytes) -> AudioFrame:
    """Parse one BLE audio notification into an AudioFrame.

    Fail Fast: a truncated header or an unknown type byte raises immediately
    with the offending length / value, rather than returning a half-built
    frame that silently corrupts the reassembled stream downstream.
    """
    if len(data) < HEADER_LEN:
        raise AudioFrameError(
            f"frame shorter than {HEADER_LEN}-byte header: got {len(data)} bytes"
        )
    frame_type, seq, timestamp_ms = struct.unpack_from("<BHI", data, 0)
    if frame_type not in (TYPE_STREAM_START, TYPE_AUDIO_CHUNK, TYPE_STREAM_END):
        raise AudioFrameError(f"unknown frame type {frame_type:#04x}")
    payload = data[HEADER_LEN:]
    if frame_type == TYPE_AUDIO_CHUNK and len(payload) % BYTES_PER_SAMPLE != 0:
        raise AudioFrameError(
            f"AUDIO_CHUNK payload {len(payload)} bytes is not a whole number "
            f"of {BYTES_PER_SAMPLE}-byte PCM samples"
        )
    return AudioFrame(
        type=frame_type, seq=seq, timestamp_ms=timestamp_ms, payload=payload
    )


@dataclass
class ReassembledStream:
    """Result of reassembling one push-to-talk stream from received frames."""

    pcm: bytes
    sample_rate: int
    # seq numbers the firmware never sent (notify gaps). Empty == clean.
    missing_seqs: list[int] = field(default_factory=list)
    # seq numbers that arrived more than once (kept first, dropped rest).
    duplicate_seqs: list[int] = field(default_factory=list)
    # True when a STREAM_END arrived; its total_chunks matched received chunks.
    complete: bool = False


def reassemble(frames: list[AudioFrame]) -> ReassembledStream:
    """Reorder/dedup audio frames by seq and concatenate PCM in seq order.

    This is the function Codex flagged as the missing-character root cause:
    CoreBluetooth notify callbacks arrive unevenly and can reorder or repeat,
    so concatenating in arrival order corrupts the audio. We sort by seq,
    keep the first copy of each seq, and report gaps and duplicates so the
    Go/No-Go statistics can prove the link was clean.

    Pure function: no BLE, no disk. Phase 2 feeds the returned pcm to STT.
    """
    start: Optional[AudioFrame] = None
    end: Optional[AudioFrame] = None
    chunks_by_seq: dict[int, AudioFrame] = {}
    duplicate_seqs: list[int] = []

    for frame in frames:
        if frame.type == TYPE_STREAM_START:
            start = frame
        elif frame.type == TYPE_STREAM_END:
            end = frame
        else:  # AUDIO_CHUNK
            if frame.seq in chunks_by_seq:
                duplicate_seqs.append(frame.seq)
                continue  # keep first arrival, drop the repeat
            chunks_by_seq[frame.seq] = frame

    if start is None:
        raise AudioFrameError("no STREAM_START frame in stream")

    sample_rate = start.sample_rate  # validates payload length

    ordered_seqs = sorted(chunks_by_seq)
    pcm = b"".join(chunks_by_seq[s].payload for s in ordered_seqs)

    # Chunk seqs run contiguously from start.seq+1. Any seq in that span with
    # no frame is a notify gap (lost audio).
    missing_seqs: list[int] = []
    if ordered_seqs:
        first_chunk_seq = start.seq + 1
        last_chunk_seq = ordered_seqs[-1]
        present = set(ordered_seqs)
        missing_seqs = [
            s for s in range(first_chunk_seq, last_chunk_seq + 1) if s not in present
        ]

    complete = False
    if end is not None:
        complete = end.total_chunks == len(chunks_by_seq) and not missing_seqs

    return ReassembledStream(
        pcm=pcm,
        sample_rate=sample_rate,
        missing_seqs=missing_seqs,
        duplicate_seqs=duplicate_seqs,
        complete=complete,
    )


@dataclass
class StreamStats:
    """BLE-link health for one push-to-talk stream — the Go/No-Go evidence."""

    frames: int
    chunks: int
    missing_count: int
    duplicate_count: int
    bytes_pcm: int
    sample_rate: int
    duration_s: float          # wall-clock from first to last frame arrival
    throughput_kbps: float     # payload bytes/s over the air
    p95_inter_arrival_ms: float
    drain_ms: float            # release (stream_end) → last frame arrival
    # Adjacent-block timestamp gaps from firmware (OV#1 sampling-hole check).
    fw_block_gap_ms_p95: float


class AudioCapture:
    """Collects audio frames from BLE notifications for one push-to-talk
    stream, then hands back the reassembled PCM. This is the Phase-2-reusable
    receive driver: feed() in the notify callback, finish() on stream_end.

    Arrival timestamps are captured here (not pure) because jitter/drain are
    properties of the live link; reassemble() stays pure for unit testing.
    """

    def __init__(self, clock: Callable[[], float] = time.monotonic) -> None:
        self._clock = clock
        self._frames: list[AudioFrame] = []
        self._arrivals: list[float] = []      # monotonic seconds per frame
        self._fw_timestamps: list[int] = []   # firmware sampling ms per frame
        self._started = False
        self._ended = False

    @property
    def ended(self) -> bool:
        return self._ended

    def feed(self, raw: bytes) -> None:
        """Parse and store one BLE notification. Raises AudioFrameError on a
        malformed frame (Fail Fast — a corrupt frame means the link or the
        firmware encoder is broken; don't silently swallow it)."""
        frame = parse_frame(raw)
        now = self._clock()
        if frame.type == TYPE_STREAM_START:
            self._started = True
        self._frames.append(frame)
        self._arrivals.append(now)
        self._fw_timestamps.append(frame.timestamp_ms)
        if frame.type == TYPE_STREAM_END:
            self._ended = True

    def finish(self) -> tuple[ReassembledStream, StreamStats]:
        """Reassemble PCM and compute link stats. Call after stream_end."""
        stream = reassemble(self._frames)
        stats = self._compute_stats(stream)
        return stream, stats

    def _compute_stats(self, stream: ReassembledStream) -> StreamStats:
        n = len(self._frames)
        duration_s = (self._arrivals[-1] - self._arrivals[0]) if n > 1 else 0.0
        inter = [
            (self._arrivals[i] - self._arrivals[i - 1]) * 1000.0
            for i in range(1, n)
        ]
        chunks = sum(1 for f in self._frames if f.type == TYPE_AUDIO_CHUNK)
        throughput = (len(stream.pcm) / duration_s / 1000.0) if duration_s > 0 else 0.0

        # drain = stream_end arrival − last audio_chunk arrival. With in-order
        # arrival the end frame is last; if a tail chunk lagged, this is small
        # or negative, which itself flags reordering.
        drain_ms = 0.0
        end_idx = next(
            (i for i, f in enumerate(self._frames) if f.type == TYPE_STREAM_END), None
        )
        last_chunk_idx = next(
            (i for i in range(n - 1, -1, -1)
             if self._frames[i].type == TYPE_AUDIO_CHUNK), None
        )
        if end_idx is not None and last_chunk_idx is not None:
            drain_ms = (self._arrivals[end_idx] - self._arrivals[last_chunk_idx]) * 1000.0

        # Firmware-side adjacent block-timestamp gaps (sampling-hole detector).
        fw_gaps = [
            self._fw_timestamps[i] - self._fw_timestamps[i - 1] for i in range(1, n)
        ]

        return StreamStats(
            frames=n,
            chunks=chunks,
            missing_count=len(stream.missing_seqs),
            duplicate_count=len(stream.duplicate_seqs),
            bytes_pcm=len(stream.pcm),
            sample_rate=stream.sample_rate,
            duration_s=duration_s,
            throughput_kbps=throughput * 8,  # KB/s → kbps
            p95_inter_arrival_ms=_percentile(inter, 95),
            drain_ms=drain_ms,
            fw_block_gap_ms_p95=_percentile([float(g) for g in fw_gaps], 95),
        )


def _percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    k = (len(ordered) - 1) * (pct / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    frac = k - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac

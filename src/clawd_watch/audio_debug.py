"""Phase-1 throwaway debug scaffold: write captured push-to-talk audio to a
wav file and log the Go/No-Go link statistics.

This is NOT reusable Phase-2 code — STT in Phase 2 consumes the reassembled
PCM directly from AudioCapture.finish(). This file exists only so a human can
listen to the wav and read the loss/jitter numbers during the BLE-throughput
go/no-go experiment. Delete it once the link is proven.
"""
from __future__ import annotations

import logging
import wave
from pathlib import Path

from .audio_receiver import AudioCapture, ReassembledStream, StreamStats

log = logging.getLogger("clawd_watch.audio_debug")

DEBUG_WAV_DIR = Path.home() / ".claude" / "clawd-watch-audio"


def write_debug_wav(stream: ReassembledStream, path: Path) -> None:
    """Write reassembled 16-bit mono PCM to a wav at the firmware-reported
    sample rate. Using the reported rate (not a hardcoded 8000) is what keeps
    a 44.1k-fallback recording from playing back at the wrong speed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(stream.sample_rate)
        w.writeframes(stream.pcm)


def log_stats(stats: StreamStats, wav_path: Path) -> None:
    """One-line Go/No-Go readout. Compare against plan thresholds:
    missing=0, drain<300ms, throughput>~50% headroom, fw_block_gap≈block size."""
    log.info(
        "AUDIO go/no-go | wav=%s | frames=%d chunks=%d | "
        "missing=%d dup=%d | %dB @ %dHz | dur=%.2fs thru=%.0fkbps | "
        "p95_iat=%.1fms drain=%.1fms | fw_block_gap_p95=%.1fms %s",
        wav_path.name,
        stats.frames, stats.chunks,
        stats.missing_count, stats.duplicate_count,
        stats.bytes_pcm, stats.sample_rate,
        stats.duration_s, stats.throughput_kbps,
        stats.p95_inter_arrival_ms, stats.drain_ms,
        stats.fw_block_gap_ms_p95,
        "OK" if stats.missing_count == 0 and stats.drain_ms < 300 else "FAIL",
    )


def finish_and_dump(capture: AudioCapture, wav_path: Path) -> StreamStats:
    """Reassemble, write the wav, log stats. Returns stats for the caller."""
    stream, stats = capture.finish()
    write_debug_wav(stream, wav_path)
    log_stats(stats, wav_path)
    return stats

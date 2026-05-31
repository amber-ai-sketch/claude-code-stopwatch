"""Unit tests for the audio frame protocol + reassembly.

These two pure functions are the missing-character root cause Codex flagged:
CoreBluetooth notify callbacks reorder/repeat, so reassembly must sort by seq,
dedup, and report gaps. Hardware behaviour goes through the Go/No-Go手测; this
file pins the logic that can be tested without a device.
"""
from __future__ import annotations

import struct

import pytest

from clawd_watch.audio_receiver import (
    AudioCapture,
    AudioFrame,
    AudioFrameError,
    HEADER_LEN,
    TYPE_AUDIO_CHUNK,
    TYPE_STREAM_END,
    TYPE_STREAM_START,
    parse_frame,
    reassemble,
)


# --- frame builders (mirror what firmware audio_frame.h will emit) ---------


def _header(frame_type: int, seq: int, ts_ms: int) -> bytes:
    return struct.pack("<BHI", frame_type, seq, ts_ms)


def start_frame(seq: int = 0, ts_ms: int = 0, sample_rate: int = 8000) -> bytes:
    return _header(TYPE_STREAM_START, seq, ts_ms) + struct.pack("<I", sample_rate)


def chunk_frame(seq: int, pcm: bytes, ts_ms: int = 0) -> bytes:
    return _header(TYPE_AUDIO_CHUNK, seq, ts_ms) + pcm


def end_frame(seq: int, total_chunks: int, ts_ms: int = 0) -> bytes:
    return _header(TYPE_STREAM_END, seq, ts_ms) + struct.pack("<H", total_chunks)


def pcm(*samples: int) -> bytes:
    return struct.pack("<" + "h" * len(samples), *samples)


# --- parse_frame -----------------------------------------------------------


def test_parse_stream_start_carries_sample_rate():
    f = parse_frame(start_frame(seq=0, ts_ms=1234, sample_rate=8000))
    assert f.type == TYPE_STREAM_START
    assert f.seq == 0
    assert f.timestamp_ms == 1234
    assert f.sample_rate == 8000


def test_parse_audio_chunk_keeps_raw_pcm():
    f = parse_frame(chunk_frame(seq=1, pcm=pcm(100, -200, 300)))
    assert f.type == TYPE_AUDIO_CHUNK
    assert f.seq == 1
    assert f.payload == pcm(100, -200, 300)


def test_parse_stream_end_carries_total_chunks():
    f = parse_frame(end_frame(seq=4, total_chunks=3))
    assert f.type == TYPE_STREAM_END
    assert f.total_chunks == 3


def test_parse_truncated_header_raises_with_length():
    with pytest.raises(AudioFrameError) as exc:
        parse_frame(b"\x02\x01")  # 2 bytes, header needs 7
    assert "2 bytes" in str(exc.value)


def test_parse_unknown_type_raises():
    with pytest.raises(AudioFrameError) as exc:
        parse_frame(_header(0x09, 0, 0))
    assert "0x09" in str(exc.value)


def test_parse_odd_pcm_length_raises():
    # 3 bytes of "PCM" is not a whole number of 16-bit samples.
    with pytest.raises(AudioFrameError) as exc:
        parse_frame(_header(TYPE_AUDIO_CHUNK, 1, 0) + b"\x01\x02\x03")
    assert "PCM" in str(exc.value)


def test_sample_rate_on_non_start_frame_raises():
    f = parse_frame(chunk_frame(seq=1, pcm=pcm(1)))
    with pytest.raises(AudioFrameError):
        _ = f.sample_rate


# --- reassemble ------------------------------------------------------------


def test_reassemble_in_order_clean_stream():
    frames = [
        parse_frame(start_frame(seq=0, sample_rate=8000)),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10, 11))),
        parse_frame(chunk_frame(seq=2, pcm=pcm(20, 21))),
        parse_frame(end_frame(seq=3, total_chunks=2)),
    ]
    out = reassemble(frames)
    assert out.pcm == pcm(10, 11, 20, 21)
    assert out.sample_rate == 8000
    assert out.missing_seqs == []
    assert out.duplicate_seqs == []
    assert out.complete is True


def test_reassemble_reorders_out_of_order_arrival():
    # Chunks arrive 2 before 1 (CoreBluetooth reordering). Output must be
    # seq-ordered, not arrival-ordered.
    frames = [
        parse_frame(start_frame(seq=0)),
        parse_frame(chunk_frame(seq=2, pcm=pcm(20, 21))),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10, 11))),
        parse_frame(end_frame(seq=3, total_chunks=2)),
    ]
    out = reassemble(frames)
    assert out.pcm == pcm(10, 11, 20, 21)
    assert out.complete is True


def test_reassemble_drops_duplicate_seq_keeps_first():
    frames = [
        parse_frame(start_frame(seq=0)),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10, 11))),
        parse_frame(chunk_frame(seq=1, pcm=pcm(99, 99))),  # duplicate
        parse_frame(chunk_frame(seq=2, pcm=pcm(20, 21))),
        parse_frame(end_frame(seq=3, total_chunks=2)),
    ]
    out = reassemble(frames)
    assert out.pcm == pcm(10, 11, 20, 21)  # kept first copy of seq 1
    assert out.duplicate_seqs == [1]
    assert out.complete is True


def test_reassemble_detects_gap():
    # seq 2 never arrived (notify gap). Stream is incomplete and gap reported.
    frames = [
        parse_frame(start_frame(seq=0)),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10, 11))),
        parse_frame(chunk_frame(seq=3, pcm=pcm(30, 31))),
        parse_frame(end_frame(seq=4, total_chunks=3)),
    ]
    out = reassemble(frames)
    assert out.missing_seqs == [2]
    assert out.complete is False
    # PCM still concatenates what arrived, in order.
    assert out.pcm == pcm(10, 11, 30, 31)


def test_reassemble_without_start_raises():
    frames = [parse_frame(chunk_frame(seq=1, pcm=pcm(1)))]
    with pytest.raises(AudioFrameError):
        reassemble(frames)


def test_reassemble_missing_end_is_incomplete():
    # Stream cut off before STREAM_END (e.g. disconnect). Not complete, but
    # whatever arrived is still usable.
    frames = [
        parse_frame(start_frame(seq=0)),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10, 11))),
    ]
    out = reassemble(frames)
    assert out.complete is False
    assert out.missing_seqs == []
    assert out.pcm == pcm(10, 11)


def test_firmware_byte_layout_symmetry():
    """The exact bytes firmware audio_frame_pack emits must decode here.

    Mirror of firmware/app_claude/ble/test_audio_frame.cpp: an AUDIO_CHUNK
    with type=0x02, seq=0x0102, ts=0x04030201, payload AA BB CC DD. If either
    side's endianness/field order drifts, this fails before hardware does.
    """
    wire = bytes([0x02, 0x02, 0x01, 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD])
    f = parse_frame(wire)
    assert f.type == TYPE_AUDIO_CHUNK
    assert f.seq == 0x0102
    assert f.timestamp_ms == 0x04030201
    assert f.payload == bytes([0xAA, 0xBB, 0xCC, 0xDD])

    # STREAM_START sample_rate=8820 → LE 74 22 00 00
    start_wire = bytes([0x01, 0, 0, 0, 0, 0, 0, 0x74, 0x22, 0x00, 0x00])
    assert parse_frame(start_wire).sample_rate == 8820

    # STREAM_END total_chunks=300 → LE 2C 01
    end_wire = bytes([0x03, 0x2D, 0x01, 0, 0, 0, 0, 0x2C, 0x01])
    assert parse_frame(end_wire).total_chunks == 300


def test_reassemble_total_chunks_mismatch_not_complete():
    # END says 3 chunks but only 2 arrived (and no gap detected in the 1..2
    # span). total_chunks guard catches the tail loss the gap scan misses.
    frames = [
        parse_frame(start_frame(seq=0)),
        parse_frame(chunk_frame(seq=1, pcm=pcm(10))),
        parse_frame(chunk_frame(seq=2, pcm=pcm(20))),
        parse_frame(end_frame(seq=3, total_chunks=3)),
    ]
    out = reassemble(frames)
    assert out.complete is False


# --- AudioCapture (live receive driver + stats) ----------------------------


class FakeClock:
    """Deterministic monotonic clock: each call advances by the queued gaps."""

    def __init__(self, times):
        self._times = list(times)
        self._i = 0

    def __call__(self):
        t = self._times[min(self._i, len(self._times) - 1)]
        self._i += 1
        return t


def test_capture_clean_stream_stats():
    # Frames arrive at 0.00, 0.04, 0.08, 0.12s (40ms blocks). fw timestamps
    # also 40ms apart → no sampling holes.
    clock = FakeClock([0.0, 0.04, 0.08, 0.12])
    cap = AudioCapture(clock=clock)
    cap.feed(start_frame(seq=0, ts_ms=0, sample_rate=8820))
    cap.feed(chunk_frame(seq=1, pcm=pcm(*range(20)), ts_ms=40))
    cap.feed(chunk_frame(seq=2, pcm=pcm(*range(20)), ts_ms=80))
    cap.feed(end_frame(seq=3, total_chunks=2, ts_ms=120))
    assert cap.ended is True

    stream, stats = cap.finish()
    assert stream.complete is True
    assert stats.missing_count == 0
    assert stats.duplicate_count == 0
    assert stats.chunks == 2
    assert stats.sample_rate == 8820
    # 4 frames over 0.12s.
    assert abs(stats.duration_s - 0.12) < 1e-9
    # fw block gaps are 40,40,40 → p95 ≈ 40ms (the OV#1 hole detector).
    assert abs(stats.fw_block_gap_ms_p95 - 40.0) < 1e-6


def test_capture_drain_after_release():
    # Last chunk at 0.10s, stream_end at 0.12s → drain 20ms.
    clock = FakeClock([0.0, 0.05, 0.10, 0.12])
    cap = AudioCapture(clock=clock)
    cap.feed(start_frame(seq=0))
    cap.feed(chunk_frame(seq=1, pcm=pcm(1)))
    cap.feed(chunk_frame(seq=2, pcm=pcm(2)))
    cap.feed(end_frame(seq=3, total_chunks=2))
    _, stats = cap.finish()
    assert abs(stats.drain_ms - 20.0) < 1e-6


def test_capture_reports_gap_in_stats():
    clock = FakeClock([0.0, 0.04, 0.08, 0.12])
    cap = AudioCapture(clock=clock)
    cap.feed(start_frame(seq=0))
    cap.feed(chunk_frame(seq=1, pcm=pcm(1)))
    cap.feed(chunk_frame(seq=3, pcm=pcm(3)))  # seq 2 lost
    cap.feed(end_frame(seq=4, total_chunks=3))
    stream, stats = cap.finish()
    assert stats.missing_count == 1
    assert stream.complete is False


def test_capture_malformed_frame_raises():
    cap = AudioCapture()
    with pytest.raises(AudioFrameError):
        cap.feed(b"\x02\x01")  # truncated

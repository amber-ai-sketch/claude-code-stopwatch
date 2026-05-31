/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Push-to-talk audio recorder. Phase 1: capture mic PCM in main-loop-sized
 * blocks, downsample 44.1k → ~8820 Hz (integer /5 decimation), and stream
 * over the dedicated BLE audio characteristic as binary frames.
 *
 * SYNCHRONOUS model (deliberately the simplest thing): GetHAL().audioRecord()
 * blocks per block, then we send. Because record and send run serially on the
 * one main-loop thread, audio cannot pile up in a buffer — so there is NO ring
 * buffer here. Backpressure surfaces as ble_audio_send() returning -3 (mbuf
 * exhaustion), which aborts the stream fast rather than dropping audio
 * silently. A ring buffer / DMA background capture is the documented fallback
 * IF the Step 0.5 sampling-timeline check proves the per-tick gaps are audible.
 *
 * Each emitted frame fits one BLE notification (≤ MTU-3), so a single block
 * may be split into several AUDIO_CHUNK frames, each with its own seq.
 */
#pragma once
#include <stdint.h>
#include <vector>

namespace clawd_watch {

class AudioRecorder {
public:
    // Begin a push-to-talk stream: sends STREAM_START with the effective
    // sample rate. No-op (returns false) if no audio subscriber.
    bool start(uint32_t now_ms);

    // Capture + send one main-loop block while the button is held. Call every
    // tick during recording. Returns false and ends the stream on backpressure
    // (send failure) or when the 30s cap is hit.
    bool tick(uint32_t now_ms);

    // End the stream: sends STREAM_END with the total chunk count.
    void stop(uint32_t now_ms);

    bool is_recording() const { return _recording; }

private:
    bool emit_pcm(const int16_t* samples, size_t count, uint32_t ts_ms);

    bool     _recording = false;
    uint16_t _seq = 0;            // frame counter: start=0, chunks=1.., end=last
    uint16_t _chunk_count = 0;    // AUDIO_CHUNK frames sent this stream
    uint32_t _start_ms = 0;
    std::vector<int16_t> _record_buf;  // reused per tick to avoid realloc

    // Native HAL rate is 44100; integer /5 decimation → 8820 Hz. Reported
    // verbatim in STREAM_START so the daemon writes a correctly-paced wav.
    static constexpr int    kDecimation     = 5;
    static constexpr uint32_t kNativeRate   = 44100;
    static constexpr uint32_t kEffectiveRate = kNativeRate / kDecimation;  // 8820
    // Per-tick native capture window. 40ms keeps each tick short so LVGL /
    // heartbeat still run; after /5 decimation that's ~352 samples/block.
    static constexpr uint16_t kBlockMs      = 40;
    static constexpr uint32_t kMaxRecordMs  = 30000;  // 30s hard cap
};

}  // namespace clawd_watch

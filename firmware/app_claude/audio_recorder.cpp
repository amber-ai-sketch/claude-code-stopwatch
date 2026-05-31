/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "audio_recorder.h"
#include "ble/audio_frame.h"
#include "ble/ble_nus.h"
#include "esp_log.h"
#include <hal/hal.h>

static const char* TAG = "audio_rec";

namespace clawd_watch {

// Largest payload that still fits one BLE notification: MTU - 3 (ATT notify
// header) - frame header. Capped to a sane default if MTU isn't known yet.
// This is the OV#3 throughput constraint made concrete: on macOS the
// negotiated MTU may be tiny, so a block is split across several frames.
static size_t max_chunk_payload()
{
    uint16_t mtu = ble_nus_current_mtu();
    if (mtu < 23) mtu = 23;  // BLE minimum
    size_t avail = (size_t)mtu - 3 - AUDIO_FRAME_HEADER_LEN;
    // Keep payload an even number of bytes (whole int16 PCM samples).
    return avail & ~(size_t)1;
}

bool AudioRecorder::start(uint32_t now_ms)
{
#ifdef AUDIO_PROBE
    // Probe mode records straight to the serial log; no BLE peer needed.
    _recording   = true;
    _seq         = 0;
    _chunk_count = 0;
    _start_ms    = now_ms;
    ESP_LOGI(TAG, "PROBE start (no BLE, serial timeline only)");
    return true;
#endif
    if (!ble_audio_is_subscribed()) {
        ESP_LOGW(TAG, "start ignored: no audio subscriber");
        return false;
    }
    _recording    = true;
    _seq          = 0;
    _chunk_count  = 0;
    _start_ms     = now_ms;

    uint8_t frame[AUDIO_FRAME_HEADER_LEN + 4];
    uint32_t rate = kEffectiveRate;
    uint8_t rate_le[4] = {
        (uint8_t)(rate & 0xff), (uint8_t)((rate >> 8) & 0xff),
        (uint8_t)((rate >> 16) & 0xff), (uint8_t)((rate >> 24) & 0xff)};
    size_t n = audio_frame_pack(frame, sizeof(frame),
                                AUDIO_STREAM_START, _seq++, now_ms, rate_le, 4);
    int rc = ble_audio_send(frame, n);
    ESP_LOGI(TAG, "stream_start rate=%u rc=%d", (unsigned)rate, rc);
    if (rc != 0) { _recording = false; return false; }
    return true;
}

bool AudioRecorder::tick(uint32_t now_ms)
{
    if (!_recording) return false;

    if (now_ms - _start_ms >= kMaxRecordMs) {
        ESP_LOGW(TAG, "30s cap hit, force stop");
        stop(now_ms);
        return false;
    }

#ifdef AUDIO_PROBE
    // Step 0.5 sampling-timeline gate (OV#1): record blocks WITHOUT sending
    // over BLE, log the real gap between consecutive captures + sample count
    // + RMS energy. If inter-block gap >> kBlockMs the synchronous main-loop
    // model is dropping audio between ticks and we must move to DMA capture.
    static uint32_t s_probe_last_ms = 0;
    GetHAL().audioRecord(_record_buf, kBlockMs);
    uint32_t gap = now_ms - s_probe_last_ms;
    s_probe_last_ms = now_ms;
    double sumsq = 0;
    for (int16_t s : _record_buf) sumsq += (double)s * s;
    int rms = _record_buf.empty() ? 0 : (int)__builtin_sqrt(sumsq / _record_buf.size());
    ESP_LOGI(TAG, "PROBE gap=%ums samples=%u rms=%d (expect gap~%ums)",
             (unsigned)gap, (unsigned)_record_buf.size(), rms, (unsigned)kBlockMs);
    return true;
#endif

    // Blocking capture of one native-rate block, then /5 decimate.
    GetHAL().audioRecord(_record_buf, kBlockMs);
    if (_record_buf.empty()) return true;  // transient read miss, keep going

    // Integer decimation 44.1k → 8820: keep every 5th sample. No anti-alias
    // filter in Phase 1 — the Go/No-Go listen test decides if it's needed.
    std::vector<int16_t> down;
    down.reserve(_record_buf.size() / kDecimation + 1);
    for (size_t i = 0; i < _record_buf.size(); i += kDecimation) {
        down.push_back(_record_buf[i]);
    }

    return emit_pcm(down.data(), down.size(), now_ms);
}

bool AudioRecorder::emit_pcm(const int16_t* samples, size_t count, uint32_t ts_ms)
{
    const size_t max_payload = max_chunk_payload();
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(samples);
    size_t total_bytes = count * sizeof(int16_t);
    size_t off = 0;

    // One block may exceed one notification; split into MTU-sized frames,
    // each its own seq so the daemon can detect gaps at frame granularity.
    while (off < total_bytes) {
        size_t take = total_bytes - off;
        if (take > max_payload) take = max_payload;

        uint8_t frame[256];  // header + payload; max_payload is MTU-bounded
        size_t n = audio_frame_pack(frame, sizeof(frame),
                                    AUDIO_CHUNK, _seq, ts_ms, bytes + off, take);
        if (n == 0) {
            ESP_LOGE(TAG, "frame pack overflow (take=%u)", (unsigned)take);
            stop(ts_ms);
            return false;
        }
        int rc = ble_audio_send(frame, n);
        if (rc != 0) {
            // Backpressure or link error: fail fast, don't drop silently.
            ESP_LOGE(TAG, "audio_send rc=%d at seq=%u — aborting stream", rc, _seq);
            stop(ts_ms);
            return false;
        }
        _seq++;
        _chunk_count++;
        off += take;
    }
    return true;
}

void AudioRecorder::stop(uint32_t now_ms)
{
    if (!_recording) return;
    _recording = false;

    uint8_t frame[AUDIO_FRAME_HEADER_LEN + 2];
    uint8_t total_le[2] = {
        (uint8_t)(_chunk_count & 0xff), (uint8_t)((_chunk_count >> 8) & 0xff)};
    size_t n = audio_frame_pack(frame, sizeof(frame),
                                AUDIO_STREAM_END, _seq, now_ms, total_le, 2);
    int rc = ble_audio_send(frame, n);
    ESP_LOGI(TAG, "stream_end chunks=%u rc=%d", (unsigned)_chunk_count, rc);
}

}  // namespace clawd_watch

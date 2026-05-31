/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Audio streaming frame format — shared wire contract with the Mac daemon
 * (src/clawd_watch/audio_receiver.py). Push-to-talk PCM streams over a
 * DEDICATED BLE audio characteristic, never the newline-JSON NUS channel
 * (audio bytes routinely contain 0x0A).
 *
 * Frame layout (little-endian), must match audio_receiver.py byte-for-byte:
 *
 *   ┌────────┬──────────┬─────────────┬──────────────────────────────┐
 *   │ type   │ seq      │ timestamp   │ payload                      │
 *   │ 1 byte │ 2 bytes  │ 4 bytes     │ variable                     │
 *   └────────┴──────────┴─────────────┴──────────────────────────────┘
 *     type:      0x01 STREAM_START, 0x02 AUDIO_CHUNK, 0x03 STREAM_END
 *     seq:       uint16 LE, per-stream frame counter (start=0, chunks=1..N,
 *                end=N+1). daemon detects gaps / reorders / dedups by it.
 *     timestamp: uint32 LE, esp_timer sampling-instant ms. Phase-1 use =
 *                detect sampling-timeline holes. NOT for STT alignment.
 *     payload:   STREAM_START → uint32 LE sample_rate
 *                AUDIO_CHUNK  → raw int16 LE PCM
 *                STREAM_END   → uint16 LE total_chunks
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace clawd_watch {

enum AudioFrameType : uint8_t {
    AUDIO_STREAM_START = 0x01,
    AUDIO_CHUNK        = 0x02,
    AUDIO_STREAM_END   = 0x03,
};

static constexpr size_t AUDIO_FRAME_HEADER_LEN = 7;  // type(1)+seq(2)+ts(4)

// Pack a frame header + payload into out. Returns total byte length, or 0 if
// out_cap can't hold header + payload_len. Little-endian on the wire; we write
// bytes explicitly rather than memcpy a struct so endianness is not host-
// dependent (ESP32 is LE, but the contract is explicit).
inline size_t audio_frame_pack(uint8_t* out, size_t out_cap,
                               uint8_t type, uint16_t seq, uint32_t ts_ms,
                               const uint8_t* payload, size_t payload_len)
{
    if (out_cap < AUDIO_FRAME_HEADER_LEN + payload_len) return 0;
    out[0] = type;
    out[1] = (uint8_t)(seq & 0xff);
    out[2] = (uint8_t)((seq >> 8) & 0xff);
    out[3] = (uint8_t)(ts_ms & 0xff);
    out[4] = (uint8_t)((ts_ms >> 8) & 0xff);
    out[5] = (uint8_t)((ts_ms >> 16) & 0xff);
    out[6] = (uint8_t)((ts_ms >> 24) & 0xff);
    if (payload_len) memcpy(out + AUDIO_FRAME_HEADER_LEN, payload, payload_len);
    return AUDIO_FRAME_HEADER_LEN + payload_len;
}

}  // namespace clawd_watch

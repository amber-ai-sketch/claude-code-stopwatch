// Host-side test for audio_frame_pack. Build:
//   c++ -std=c++17 -I.. test_audio_frame.cpp -o /tmp/aframe_test && /tmp/aframe_test
// Not part of the firmware build — ESP_PLATFORM gate excludes it (main/
// CMakeLists.txt uses GLOB_RECURSE).
//
// Verifies the wire bytes match the contract the daemon parser
// (src/clawd_watch/audio_receiver.py) decodes: 7-byte LE header
// [type|seq(2)|ts(4)] + payload. If this drifts from the Python side, the
// daemon decodes noise — this test is the symmetry guard.
#ifndef ESP_PLATFORM
#include "audio_frame.h"
#include <assert.h>
#include <stdio.h>

using namespace clawd_watch;

int main() {
    int passed = 0, failed = 0;
    auto check = [&](const char* label, bool ok) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", label);
        if (ok) passed++; else failed++;
    };

    // --- header byte layout: type=0x02, seq=0x0102, ts=0x04030201 ---
    {
        uint8_t pcm[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        uint8_t out[32];
        size_t n = audio_frame_pack(out, sizeof(out), AUDIO_CHUNK,
                                    0x0102, 0x04030201, pcm, 4);
        check("returns header+payload length", n == AUDIO_FRAME_HEADER_LEN + 4);
        check("type byte", out[0] == 0x02);
        check("seq LE low/high", out[1] == 0x02 && out[2] == 0x01);
        check("ts LE bytes",
              out[3] == 0x01 && out[4] == 0x02 && out[5] == 0x03 && out[6] == 0x04);
        check("payload copied verbatim",
              out[7] == 0xAA && out[8] == 0xBB && out[9] == 0xCC && out[10] == 0xDD);
    }

    // --- STREAM_START with 4-byte sample_rate=8820 (0x2274 LE) ---
    {
        uint32_t rate = 8820;
        uint8_t rate_le[4] = {(uint8_t)(rate), (uint8_t)(rate >> 8),
                              (uint8_t)(rate >> 16), (uint8_t)(rate >> 24)};
        uint8_t out[16];
        size_t n = audio_frame_pack(out, sizeof(out), AUDIO_STREAM_START,
                                    0, 0, rate_le, 4);
        check("start frame length", n == AUDIO_FRAME_HEADER_LEN + 4);
        check("start type", out[0] == 0x01);
        // 8820 = 0x2274 → LE bytes 0x74 0x22 0x00 0x00
        check("sample_rate LE", out[7] == 0x74 && out[8] == 0x22 &&
                                out[9] == 0x00 && out[10] == 0x00);
    }

    // --- STREAM_END with 2-byte total_chunks=300 (0x012C LE) ---
    {
        uint16_t total = 300;
        uint8_t total_le[2] = {(uint8_t)total, (uint8_t)(total >> 8)};
        uint8_t out[16];
        size_t n = audio_frame_pack(out, sizeof(out), AUDIO_STREAM_END,
                                    301, 0, total_le, 2);
        check("end frame length", n == AUDIO_FRAME_HEADER_LEN + 2);
        check("end type", out[0] == 0x03);
        check("total_chunks LE", out[7] == 0x2C && out[8] == 0x01);
    }

    // --- capacity guard: refuses to overflow ---
    {
        uint8_t pcm[8] = {0};
        uint8_t out[8];  // too small for 7-byte header + 8 payload
        size_t n = audio_frame_pack(out, sizeof(out), AUDIO_CHUNK, 0, 0, pcm, 8);
        check("returns 0 on overflow", n == 0);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
#endif

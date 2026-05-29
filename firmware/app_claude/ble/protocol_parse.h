/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Parses the line-delimited JSON protocol the daemon sends over NUS:
 *   - `{"time":[epoch, tz_off]}`     time sync (M4 ignores RTC)
 *   - `{"cmd":"owner","name":"..."}` device-display owner
 *   - `{"total":...,"msg":"...",...}` heartbeat snapshot
 *
 * Usage:
 *
 *   LineBuf<1024> rx;
 *   WatchState state;
 *   ...
 *   // Each time NUS receives bytes:
 *   rx.feed(bytes, n, [&](const char* line) {
 *       apply_json_line(line, state);
 *   });
 */
#pragma once
#include "watch_state.h"
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

namespace clawd_watch {

// Line accumulator: feed bytes, callback fires once per \n-terminated
// line. Buffer overflow truncates the line silently.
template<size_t N>
class LineBuf {
public:
    template<typename F>
    void feed(const uint8_t* data, size_t n, F&& on_line) {
        for (size_t i = 0; i < n; i++) {
            char c = (char)data[i];
            if (c == '\n' || c == '\r') {
                if (_len > 0) {
                    _buf[_len] = 0;
                    on_line(_buf);
                    _len = 0;
                }
            } else if (_len < N - 1) {
                _buf[_len++] = c;
            }
        }
    }

    void reset() { _len = 0; }

private:
    char     _buf[N];
    uint16_t _len = 0;
};

// Returns true if `line` was a valid heartbeat (not time/owner/etc), so
// the caller can log "got heartbeat" once. False does NOT mean error —
// commands and time-syncs return false too.
bool apply_json_line(const char* line, WatchState& state, uint32_t now_ms);

}  // namespace clawd_watch

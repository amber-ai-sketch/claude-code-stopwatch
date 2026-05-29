/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Button event FSM — turns raw GPIO level samples into discrete events:
 *   SINGLE  (press release < 200ms, no second press within 300ms)
 *   DOUBLE  (two presses within 300ms of each other)
 *   LONG    (press held > 500ms, fires once when threshold crosses)
 *
 * Two independent FSMs (left + right) live side-by-side; the watch's
 * AppClaude pumps each on its tick.
 *
 * State diagram:
 *   IDLE → (press) → PRESSED
 *   PRESSED → (release within 200ms) → WAIT_DOUBLE
 *           → (held 500ms+) → emit LONG, → LONG_HELD
 *   WAIT_DOUBLE → (300ms timeout) → emit SINGLE, → IDLE
 *               → (second press) → emit DOUBLE, → CONSUMED_DOUBLE
 *   LONG_HELD → (release) → IDLE
 *   CONSUMED_DOUBLE → (release) → IDLE
 *
 * Debounce: a 30ms hold-down cooldown prevents bouncing edges from
 * registering as rapid sequences. Implemented as a "minimum time between
 * sampled level changes" filter, not a software debounce timer.
 *
 * Single-press latency: 300ms (we have to wait the double-click window).
 * Long-press fires the moment the 500ms threshold crosses, so users feel
 * tactile confirmation. Double-click fires on the second press, so the
 * first SINGLE never gets emitted (it gets retroactively reclassified as
 * the first half of a double — easy because we delay its emission).
 */
#pragma once
#include <stdint.h>

namespace clawd_watch {

enum class ButtonEvent : uint8_t {
    None = 0,
    Single,
    Double,
    Long,
};

class ButtonFsm {
public:
    explicit ButtonFsm(const char* name = "btn") : _name(name) {}

    // Pump once per main loop tick. `pressed` is the de-bounced raw level
    // (true = button held down). `now_ms` is monotonic. Returns the event
    // produced this tick (often None).
    ButtonEvent update(bool pressed, uint32_t now_ms);

    // Diagnostics.
    const char* name() const { return _name; }

private:
    enum class S : uint8_t {
        Idle,
        Pressed,         // currently held down, < LongMs
        WaitDouble,      // released after < SingleMaxMs, waiting for second press
        LongHeld,        // emitted Long, still held — wait for release
        ConsumedDouble,  // emitted Double, second press still held — wait for release
    };

    static constexpr uint32_t kDebounceMs   = 30;
    static constexpr uint32_t kSingleMaxMs  = 200;  // press shorter than this is candidate Single
    static constexpr uint32_t kDoubleMaxMs  = 300;  // gap between presses to count as Double
    static constexpr uint32_t kLongMs       = 500;

    const char* _name;
    S        _state           = S::Idle;
    bool     _last_pressed    = false;
    bool     _saw_transition  = false;  // gate debounce filter on first edge
    uint32_t _last_change_ms  = 0;
    uint32_t _press_start_ms  = 0;
    uint32_t _release_ms      = 0;
};

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Bridges button GPIO state → ButtonFsm events → BLE HID keystrokes.
 *
 * Tick once per main loop. Holds long-press behavior across ticks
 * (Shift+Space stays pressed while right button is held, releases on
 * release) since ButtonFsm only emits Long once at threshold crossing.
 *
 * M5 minimal mapping (only right-button long-press is wired for now):
 *   right long-press   → Shift+Space hold while held, release on lift
 *   everything else    → not yet
 */
#pragma once
#include <stdint.h>

namespace clawd_watch {

class HidDispatcher {
public:
    // Pump once per main loop tick. left_pressed / right_pressed are the
    // raw debounced GPIO levels.
    void tick(bool left_pressed, bool right_pressed, uint32_t now_ms);

private:
    bool _right_was_pressed = false;
    bool _shift_space_held = false;
    uint32_t _right_press_start_ms = 0;
    static constexpr uint32_t kLongMs = 500;
};

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Bridges right-button level → NUS JSON key commands for the Mac daemon.
 *
 * Tick once per main loop. Holds long-press behavior across ticks
 * (Shift+Space stays pressed while right button is held, releases on
 * release).
 *
 * Minimal mapping (only right-button long-press is wired for now):
 *   right long-press   → key_down shift+space, key_up on lift
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

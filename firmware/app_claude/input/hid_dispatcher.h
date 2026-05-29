/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Bridges button presses → NUS JSON key commands for the Mac daemon.
 *
 * Tick once per main loop. Mapping:
 *   right long-press  → hold left Shift+Space (WeChat dictation), key_up on lift
 *   right short-click → Esc
 *   left  single      → Backspace
 *   left  long-press  → Return
 *
 * Right button uses direct level tracking because dictation needs
 * press-hold-release semantics (Shift+Space held while down). Left button
 * uses ButtonFsm since its actions are one-shot taps.
 */
#pragma once
#include <stdint.h>
#include "button_fsm.h"

namespace clawd_watch {

class HidDispatcher {
public:
    // Pump once per main loop tick. left_pressed / right_pressed are the
    // raw debounced GPIO levels.
    void tick(bool left_pressed, bool right_pressed, uint32_t now_ms);

private:
    // Right button: level tracking for hold-to-talk + short-click Esc.
    bool _right_was_pressed = false;
    bool _shift_space_held = false;
    bool _right_long_fired = false;  // long threshold crossed this press
    uint32_t _right_press_start_ms = 0;
    static constexpr uint32_t kLongMs = 500;

    // Left button: FSM-driven one-shot taps.
    ButtonFsm _left_fsm{"left"};
};

}  // namespace clawd_watch

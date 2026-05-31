/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Bridges button presses → NUS JSON commands for the Mac daemon.
 *
 * Tick once per main loop. Mapping:
 *   right long-press  → raw "btn right down/up" event; the daemon decides
 *                       what it means per current mode (trigger → WeChat
 *                       Shift+Space; mic → record). No mode state on-device.
 *   right short-click → Esc
 *   left  single      → Backspace
 *   left  long-press  → Return
 *
 * Right button uses direct level tracking because the long-press needs
 * press-hold-release semantics. Left button uses ButtonFsm since its
 * actions are one-shot taps.
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
    // Right button: level tracking for long-press hold + short-click Esc.
    bool _right_was_pressed = false;
    bool _right_long_held = false;    // long-press "down" sent, awaiting "up"
    bool _right_long_fired = false;   // long threshold crossed this press
    uint32_t _right_press_start_ms = 0;
    static constexpr uint32_t kLongMs = 500;

    // Left button: FSM-driven one-shot taps.
    ButtonFsm _left_fsm{"left"};
};

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "button_fsm.h"

namespace clawd_watch {

ButtonEvent ButtonFsm::update(bool pressed, uint32_t now_ms)
{
    // No debounce here — Button_Class (HAL) already debounces at 10ms
    // before we see the level. Adding a second debounce would double-filter.

    switch (_state) {
        case S::Idle:
            if (pressed) {
                _state          = S::Pressed;
                _press_start_ms = now_ms;
            }
            return ButtonEvent::None;

        case S::Pressed: {
            uint32_t held = now_ms - _press_start_ms;
            if (pressed) {
                if (held >= kLongMs) {
                    // Fire LONG immediately on threshold cross — user feels it.
                    _state = S::LongHeld;
                    return ButtonEvent::Long;
                }
                return ButtonEvent::None;
            }
            // Released. Was it short enough to be a Single candidate?
            if (held < kSingleMaxMs) {
                _state      = S::WaitDouble;
                _release_ms = now_ms;
                return ButtonEvent::None;
            }
            // Released after kSingleMaxMs but before kLongMs — neither
            // single nor long. Eat it. (This is the "medium press"
            // dead zone; users learn to press shorter or longer.)
            _state = S::Idle;
            return ButtonEvent::None;
        }

        case S::WaitDouble:
            if (pressed) {
                // Second press inside kDoubleMaxMs window → DOUBLE.
                if (now_ms - _release_ms <= kDoubleMaxMs) {
                    _state = S::ConsumedDouble;
                    return ButtonEvent::Double;
                }
                // Stale press too late after the first release; treat as
                // a brand-new press (emit deferred SINGLE for the prior,
                // then fall through). But emitting two events from one
                // tick would complicate the API — instead we'll lose
                // this rare race and require the user to release first.
                _state          = S::Pressed;
                _press_start_ms = now_ms;
                return ButtonEvent::Single;
            }
            // Still released. Has the double-click window expired?
            if (now_ms - _release_ms > kDoubleMaxMs) {
                _state = S::Idle;
                return ButtonEvent::Single;
            }
            return ButtonEvent::None;

        case S::LongHeld:
            // Wait for release. Don't emit anything more.
            if (!pressed) _state = S::Idle;
            return ButtonEvent::None;

        case S::ConsumedDouble:
            // Wait for the second press to be released.
            if (!pressed) _state = S::Idle;
            return ButtonEvent::None;
    }
    return ButtonEvent::None;
}

}  // namespace clawd_watch

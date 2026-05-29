/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Right-button long-press mapping for M5.
 *
 * Why this isn't using ButtonFsm yet: ButtonFsm emits a single Long
 * event when threshold crosses, but we need press+hold+release semantics
 * (Shift+Space pressed continuously while held, released on lift) so
 * WeChat IME enters dictation on the leading edge and exits on the
 * trailing edge. Direct level tracking is simpler than threading state
 * through FSM events. M3 FSM still drives single/double click + left
 * button events later.
 */
#include "hid_dispatcher.h"
#include "../ble/ble_hid.h"
#include "esp_log.h"

static const char* TAG = "hid_dispatch";

namespace clawd_watch {

void HidDispatcher::tick(bool /*left_pressed*/, bool right_pressed, uint32_t now_ms)
{
    // Track right-button level. Press starts a timer, hold past 500ms
    // engages Shift+Space, release always disengages.
    if (right_pressed && !_right_was_pressed) {
        _right_press_start_ms = now_ms;
    }

    if (right_pressed && !_shift_space_held &&
        (now_ms - _right_press_start_ms >= kLongMs)) {
        // Crossed long threshold — send Shift+Space and remember.
        int rc = ble_hid_press(HID_MOD_LSHIFT, HID_KEY_SPACE);
        ESP_LOGI(TAG, "Shift+Space press (rc=%d)", rc);
        _shift_space_held = true;
    }

    if (!right_pressed && _shift_space_held) {
        int rc = ble_hid_release();
        ESP_LOGI(TAG, "Shift+Space release (rc=%d)", rc);
        _shift_space_held = false;
    }

    _right_was_pressed = right_pressed;
}

}  // namespace clawd_watch

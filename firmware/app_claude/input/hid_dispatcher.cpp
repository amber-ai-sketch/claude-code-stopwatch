/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Right-button long-press mapping for M5.
 *
 * Sends NUS JSON commands (not BLE HID): macOS rejects HID input from a
 * BLE-HID device that wasn't paired through the Bluetooth UI, so the Mac
 * daemon injects the keystroke locally via Quartz instead. The device
 * just tells it which key to press.
 *
 * Why this isn't using ButtonFsm: ButtonFsm emits a single Long event
 * when threshold crosses, but we need press+hold+release semantics
 * (Shift+Space pressed continuously while held, released on lift) so
 * WeChat IME enters dictation on the leading edge and exits on the
 * trailing edge. Direct level tracking is simpler than threading state
 * through FSM events.
 */
#include "hid_dispatcher.h"
#include "../ble/ble_nus.h"
#include "esp_log.h"
#include <string.h>

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
        // Crossed long threshold — tell the daemon to hold left Shift+Space.
        const char* msg = "{\"cmd\":\"key_down\",\"mod\":\"shift\",\"key\":\"space\"}";
        int rc = ble_nus_send(msg, strlen(msg));
        ESP_LOGI(TAG, "key_down shift+space (rc=%d)", rc);
        _shift_space_held = true;
    }

    if (!right_pressed && _shift_space_held) {
        const char* msg = "{\"cmd\":\"key_up\",\"mod\":\"shift\",\"key\":\"space\"}";
        int rc = ble_nus_send(msg, strlen(msg));
        ESP_LOGI(TAG, "key_up shift+space (rc=%d)", rc);
        _shift_space_held = false;
    }

    _right_was_pressed = right_pressed;
}

}  // namespace clawd_watch

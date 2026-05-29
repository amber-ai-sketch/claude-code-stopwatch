/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Button → NUS JSON key command mapping for M5.
 *
 * Sends NUS JSON commands (not BLE HID): macOS rejects HID input from a
 * BLE-HID device that wasn't paired through the Bluetooth UI, so the Mac
 * daemon injects the keystroke locally via Quartz instead. The device
 * just tells it which key to press.
 *
 * Right button uses direct level tracking (not ButtonFsm) because
 * hold-to-talk needs press+hold+release: Shift+Space held continuously
 * while down so WeChat IME records on the leading edge and stops on the
 * trailing edge. A short click (released before the long threshold) sends
 * Esc instead. Left button uses ButtonFsm since its actions are one-shot.
 */
#include "hid_dispatcher.h"
#include "../ble/ble_nus.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "hid_dispatch";

namespace clawd_watch {

static void send_cmd(const char* json)
{
    int rc = ble_nus_send(json, strlen(json));
    ESP_LOGI(TAG, "send %s (rc=%d)", json, rc);
}

void HidDispatcher::tick(bool left_pressed, bool right_pressed, uint32_t now_ms)
{
    // ── Right button: hold-to-talk + short-click Esc ──
    if (right_pressed && !_right_was_pressed) {
        _right_press_start_ms = now_ms;
        _right_long_fired     = false;
    }

    if (right_pressed && !_shift_space_held &&
        (now_ms - _right_press_start_ms >= kLongMs)) {
        // Crossed long threshold — hold left Shift+Space for dictation.
        send_cmd("{\"cmd\":\"key_down\",\"mod\":\"shift\",\"key\":\"space\"}");
        _shift_space_held = true;
        _right_long_fired = true;
    }

    if (!right_pressed && _right_was_pressed) {
        // Released.
        if (_shift_space_held) {
            send_cmd("{\"cmd\":\"key_up\",\"mod\":\"shift\",\"key\":\"space\"}");
            _shift_space_held = false;
        } else if (!_right_long_fired) {
            // Short click, never crossed long threshold → Esc.
            send_cmd("{\"cmd\":\"key_tap\",\"key\":\"esc\"}");
        }
    }

    _right_was_pressed = right_pressed;

    // ── Left button: single → Backspace, long → Return ──
    switch (_left_fsm.update(left_pressed, now_ms)) {
        case ButtonEvent::Single:
            send_cmd("{\"cmd\":\"key_tap\",\"key\":\"backspace\"}");
            break;
        case ButtonEvent::Long:
            send_cmd("{\"cmd\":\"key_tap\",\"key\":\"enter\"}");
            break;
        default:
            break;
    }
}

}  // namespace clawd_watch

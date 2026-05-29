/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * BLE HID keyboard. Standard 8-byte boot keyboard report:
 *   [modifier, reserved, key1, key2, key3, key4, key5, key6]
 *
 * macOS HID over GATT: needs Report Map + Input Report char + CCCD.
 * Lives alongside NUS in the same connection — both services are
 * registered in the same ble_gatts_add_svcs() call inside ble_nus_init,
 * so Mac sees one device with both profiles.
 *
 * API:
 *   ble_hid_init()      called once during NimBLE init (before service register)
 *   ble_hid_press(modifier, keycode)  press a key with given modifier mask
 *   ble_hid_release()                 release all keys (sends empty report)
 *
 * Modifier bits (HID 1.11 §8.3):
 *   0x01 LCtrl  0x02 LShift  0x04 LAlt  0x08 LGUI
 *   0x10 RCtrl  0x20 RShift  0x40 RAlt  0x80 RGUI
 *
 * Common keycodes (HID Usage Table §10):
 *   0x2C Space   0x2A Backspace   0x29 Esc   0x28 Enter
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LCTRL   0x01

#define HID_KEY_SPACE      0x2C
#define HID_KEY_BACKSPACE  0x2A
#define HID_KEY_ESC        0x29
#define HID_KEY_ENTER      0x28
#define HID_KEY_C          0x06

// Service definitions are added by this module to the NUS svc array.
// Returns the GATT service definitions (terminated). Append into the
// outer service list before calling ble_gatts_add_svcs.
const struct ble_gatt_svc_def* ble_hid_service_defs(void);

// Set conn handle once a connection is established. Pass 0xffff on disconnect.
void ble_hid_set_conn(uint16_t conn_handle);

// Notify subscriber bookkeeping — call from GAP_EVENT_SUBSCRIBE.
void ble_hid_on_subscribe(uint16_t attr_handle, bool subscribed);

// Send key press/release. Returns 0 on success, negative on error.
int ble_hid_press(uint8_t modifier, uint8_t keycode);
int ble_hid_release(void);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * BLE NimBLE NUS server.
 *
 * Wire protocol: Nordic UART Service over GATT.
 *   Service  : 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX char  : 6e400002-b5a3-f393-e0a9-e50e24dcca9e   (write, host → device)
 *   TX char  : 6e400003-b5a3-f393-e0a9-e50e24dcca9e   (notify, device → host)
 *
 * The Mac daemon uses these UUIDs verbatim (see protocol.py).
 *
 * Lifecycle:
 *   ble_nus_init()       once at boot, registers GATT services
 *   ble_nus_start_adv()  start advertising — call after BLE host sync
 *   ble_nus_send(json)   write line to TX (blocks if there's no subscriber)
 *
 * Set the rx callback to receive newline-delimited JSON from the daemon.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called from a NimBLE task context whenever a complete \n-terminated
// line is received on RX. The pointer is valid only for the duration of
// the callback. Length excludes the newline.
typedef void (*ble_nus_rx_line_cb)(const char* line, size_t len, void* user);

void ble_nus_init(void);
void ble_nus_start_adv(void);
void ble_nus_set_rx_callback(ble_nus_rx_line_cb cb, void* user);

// Returns true if a peer has subscribed to TX notifications.
bool ble_nus_is_connected(void);

// Send one line of JSON over TX. Adds the trailing \n if not present.
// Returns 0 on success, negative on error (no subscriber, MTU exceeded,
// or NimBLE error code).
int ble_nus_send(const char* json, size_t len);

// Last-known passkey for display during pairing. 0 means no pairing in
// flight or already complete. Read by the LVGL pairing UI.
uint32_t ble_nus_current_passkey(void);

#ifdef __cplusplus
}
#endif

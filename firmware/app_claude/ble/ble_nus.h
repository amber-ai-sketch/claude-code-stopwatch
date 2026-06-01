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

// Send raw bytes over the dedicated AUDIO TX characteristic (NOT NUS TX).
// No newline framing — the daemon parses length-delimited binary audio
// frames (see audio_frame.h). Returns 0 on success, negative on error:
//   -1 no audio subscriber, -3 mbuf alloc failed (out of buffers =
//   backpressure: send is outrunning the BLE link), or a NimBLE rc.
// The caller treats -3 as a fail-fast signal to abort the stream.
int ble_audio_send(const uint8_t* data, size_t len);

// True if a peer has subscribed to AUDIO TX notifications.
bool ble_audio_is_subscribed(void);

// Negotiated ATT MTU for the active connection, or 0 if not connected.
// The daemon-side throughput baseline depends on this; macOS often
// negotiates only 23 (= 20-byte payload), which caps audio throughput.
uint16_t ble_nus_current_mtu(void);

// Last-known passkey for display during pairing. 0 means no pairing in
// flight or already complete. Read by the LVGL pairing UI.
uint32_t ble_nus_current_passkey(void);

// Switch advertising interval. fast=true → 30-60ms (prompt discovery),
// fast=false → 1000-2000ms (power-saving while idle/disconnected).
// No-op if already in the requested mode or if currently connected.
void ble_nus_set_adv_fast(bool fast);

#ifdef __cplusplus
}
#endif

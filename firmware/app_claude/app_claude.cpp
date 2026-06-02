/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "app_claude.h"
#include "ble/ble_nus.h"
#include "ble/protocol_parse.h"
#include "ui/watch_face.h"
#include <driver/gpio.h>
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <string.h>

// Match HAL pin definitions (firmware/upstream/main/hal/hal_button.cpp).
// gpio_get_level returns 1 when not pressed (pull-up), 0 when pressed.
static constexpr gpio_num_t kBtnLeftPin  = (gpio_num_t)2;
static constexpr gpio_num_t kBtnRightPin = (gpio_num_t)1;

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace clawd_watch;

namespace {

// NUS rx callback runs in a NimBLE task — apply_json_line writes into
// the WatchState owned by the app. The main thread reads it in
// onRunning(). Trivially safe because the writes are short and the
// reads happen on the next LVGL tick at worst — slightly torn strings
// only flicker the screen, never corrupt anything.
// BLE callback also signals screen-dim activity so that state changes
// (e.g. timer start from Mac) wake the display. Static because the
// callback signature only accepts a single void* user (the WatchState).
static std::atomic<bool> s_ble_rx_activity{false};

void on_nus_rx_line(const char* line, size_t /*len*/, void* user)
{
    auto* state = static_cast<WatchState*>(user);
    if (!state || !line) return;
    apply_json_line(line, *state, 0);
    s_ble_rx_activity.store(true, std::memory_order_relaxed);
}

}  // namespace

AppClaude::AppClaude()
{
    setAppInfo().name = "Claude";
}

void AppClaude::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // BLE init is one-shot: keep it in onCreate so a future onClose →
    // onOpen cycle (e.g. settings page round-trip) doesn't tear down
    // the BLE stack and force re-pairing.
    ble_nus_init();
    ble_nus_set_rx_callback(on_nus_rx_line, &_state);
}

void AppClaude::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    {
        LvglLockGuard lock;
        _face = std::make_unique<WatchFace>();
        _face->apply(_state, ble_nus_is_connected());
        _face->show();
    }

    // Kick the screen-dim timer so it doesn't dim immediately on boot.
    _last_activity_ms = GetHAL().millis();
    _screen_dimmed    = false;
}

void AppClaude::onRunning()
{
    if (_key_manager) {
        // M3: button FSM will replace this. KeyManager kept so the demo's
        // GoHome long-press doesn't deadlock waiting on us.
        (void)_key_manager->update();
    }

    // Button state via HAL's m5::Button_Class. updateButtonStates() runs
    // inside _key_manager->update() above and refreshes btnA/btnB from
    // GPIO (KEYA=GPIO2=btnA, KEYB=GPIO1=btnB).
    bool right_pressed = GetHAL().btnB.isPressed();
    bool left_pressed  = GetHAL().btnA.isPressed();

    // Log press/release edges to trace the input → NUS path.
    static bool last_r = false, last_l = false;
    bool edge_r = (right_pressed != last_r);
    bool edge_l = (left_pressed  != last_l);
    if (edge_r) {
        mclog::tagInfo(getAppInfo().name, "right={}", (int)right_pressed);
        last_r = right_pressed;
    }
    if (edge_l) {
        mclog::tagInfo(getAppInfo().name, "left={}", (int)left_pressed);
        last_l = left_pressed;
    }

    _hid.tick(left_pressed, right_pressed, GetHAL().millis());

    // ── Screen auto-dim ──
    // Track activity from button edges, touch, and BLE data.
    // Charging = USB power present → never dim; restore if we just plugged in.
    uint32_t now = GetHAL().millis();
    bool charging = GetHAL().isBatteryCharging();
    bool touch_active = GetHAL().getTouchPoint().num > 0;
    bool has_activity = charging || edge_r || edge_l || touch_active
                     || s_ble_rx_activity.exchange(false, std::memory_order_relaxed);
    if (has_activity) {
        _last_activity_ms = now;
        if (_screen_dimmed) {
            GetHAL().setBackLightBrightness(_saved_brightness);
            _screen_dimmed = false;
        }
    } else if (!_screen_dimmed && (now - _last_activity_ms > kScreenDimMs)) {
        _saved_brightness = GetHAL().getBackLightBrightness();
        GetHAL().setBackLightBrightness(0);
        _screen_dimmed = true;
    }

    // ── BLE disconnect detection + power-saving advertising ──
    bool ble_ok = ble_nus_is_connected();
    if (ble_ok) {
        _ble_lost_since = 0;
        _adv_slow       = false;
    } else {
        if (_ble_lost_since == 0) _ble_lost_since = now;
        // After 30s disconnected, switch to slow advertising to save power.
        if (!_adv_slow && (now - _ble_lost_since > kAdvSlowMs)) {
            ble_nus_set_adv_fast(false);
            _adv_slow = true;
        }
        // User interaction while disconnected: boost advertising for 30s.
        bool user_active = edge_r || edge_l || touch_active;
        if (user_active && _adv_slow) {
            ble_nus_set_adv_fast(true);
            _adv_slow       = false;
            _ble_lost_since = now;  // restart 30s countdown
        }
    }

    // Apply BLE state to UI ~5x/s. LVGL is mutex-protected so we have to
    // grab the lock; doing this every loop iteration would stall.
    if (now - _last_ui_apply_ms > 200) {
        _last_ui_apply_ms = now;
        _state.battery_pct      = GetHAL().getBatteryLevel();
        _state.battery_charging = charging;
        if (_face) {
            LvglLockGuard lock;
            _face->apply(_state, ble_ok);
        }
    }

    // Periodic heartbeat → daemon (lets it see the watch is awake).
    // Daemon uses the inbound heartbeat as the connection liveness signal.
    if (now - _last_tick_ms > 5000) {
        _last_tick_ms = now;
        if (ble_nus_is_connected()) {
            const char* hb = "{\"cmd\":\"status\"}";
            ble_nus_send(hb, strlen(hb));
        }
    }
}

void AppClaude::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    _key_manager.reset();
    {
        LvglLockGuard lock;
        _face.reset();
    }
}

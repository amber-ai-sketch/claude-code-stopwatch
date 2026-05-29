/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "app_claude.h"
#include "ble/ble_nus.h"
#include "ble/protocol_parse.h"
#include "ui/idle_page.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <string.h>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace clawd_watch;

namespace {

// NUS rx callback runs in a NimBLE task — apply_json_line writes into
// the WatchState owned by the app. The main thread reads it in
// onRunning(). Trivially safe because the writes are short and the
// reads happen on the next LVGL tick at worst — slightly torn strings
// only flicker the screen, never corrupt anything.
void on_nus_rx_line(const char* line, size_t /*len*/, void* user)
{
    auto* state = static_cast<WatchState*>(user);
    if (!state || !line) return;
    apply_json_line(line, *state, 0);
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
        _idle_page = std::make_unique<IdlePage>();
        _idle_page->update("Claude", 0.00f, 0.0f, 0.0f, 0.0f);
        _idle_page->show();
    }
}

void AppClaude::onRunning()
{
    if (_key_manager) {
        // M3: button FSM will replace this. KeyManager kept so the demo's
        // GoHome long-press doesn't deadlock waiting on us.
        (void)_key_manager->update();
    }

    // Apply BLE state to UI ~5x/s. LVGL is mutex-protected so we have to
    // grab the lock; doing this every loop iteration would stall.
    uint32_t now = GetHAL().millis();
    if (now - _last_ui_apply_ms > 200) {
        _last_ui_apply_ms = now;
        if (_idle_page) {
            LvglLockGuard lock;
            // Show real values when present; otherwise keep zeros.
            const std::string& model = _state.model_name.empty()
                ? std::string(_state.connected ? "Claude" : "—")
                : _state.model_name;
            _idle_page->update(
                model,
                _state.cost_valid ? _state.cost_usd : 0.0f,
                _state.context_valid ? _state.context_pct : 0.0f,
                _state.rate_5h_valid ? _state.rate_5h_pct : 0.0f,
                _state.rate_7d_valid ? _state.rate_7d_pct : 0.0f
            );
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
        _idle_page.reset();
    }
}

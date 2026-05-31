/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * AppClaude — top-level mooncake app that owns the watch's Claude Code mode.
 * Wraps:
 *   - BLE NUS server (talks to clawd-watch-daemon over JSON)
 *   - BLE HID keyboard (sends Shift+Space / Backspace / etc to macOS) [M5]
 *   - Button event FSM (single/double/long, per-side) [M3]
 *   - LVGL UI pages (idle / busy / attention / sleep) [M2 + M6]
 *
 * Designed to be the only installed app on the watch — the launcher is
 * skipped in main.cpp, AppClaude opens immediately and never closes
 * (close() is wired to a no-op, IMU/touch quit gestures are disabled).
 */
#pragma once
#include "ble/watch_state.h"
#include "input/hid_dispatcher.h"
#include "ui/watch_face.h"
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>

// Forward declaration for types that don't appear in unique_ptr members yet.
namespace clawd_watch {
class StateMachine;
}  // namespace clawd_watch

class AppClaude : public mooncake::AppAbility {
public:
    AppClaude();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // Swipeable watch face: overview tile + one tile per session.
    std::unique_ptr<clawd_watch::WatchFace> _face;

    // Lightweight key manager wrapper from M5StopWatch demo. M3 will replace
    // this with a custom FSM that emits SINGLE/DOUBLE/LONG per side.
    std::unique_ptr<input::KeyManager> _key_manager;

    // Last time we ticked stale state etc.
    uint32_t _last_tick_ms = 0;

    // Live state, fed by the NUS rx callback (which runs on a NimBLE task).
    // We refresh the LVGL idle page from these on each onRunning() tick.
    clawd_watch::WatchState _state;
    uint32_t _last_ui_apply_ms = 0;

    // M5 HID input: button → NUS command dispatcher.
    clawd_watch::HidDispatcher _hid;
};

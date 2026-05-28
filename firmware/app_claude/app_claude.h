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
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>

namespace clawd_watch {
class StateMachine;
class IdlePage;
}  // namespace clawd_watch

class AppClaude : public mooncake::AppAbility {
public:
    AppClaude();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // Owned UI pages (one screen per state). M2 only ships idle.
    std::unique_ptr<clawd_watch::IdlePage> _idle_page;

    // Lightweight key manager wrapper from M5StopWatch demo. M3 will replace
    // this with a custom FSM that emits SINGLE/DOUBLE/LONG per side.
    std::unique_ptr<input::KeyManager> _key_manager;

    // Last time we ticked stale state etc.
    uint32_t _last_tick_ms = 0;
};

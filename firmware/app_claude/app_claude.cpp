/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "app_claude.h"
#include "ui/idle_page.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace clawd_watch;

AppClaude::AppClaude()
{
    setAppInfo().name = "Claude";
}

void AppClaude::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppClaude::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    {
        LvglLockGuard lock;
        _idle_page = std::make_unique<IdlePage>();
        // Placeholder values until M4 wires the BLE NUS protocol parser
        // and feeds real heartbeat data in.
        _idle_page->update("Claude", 0.00f, 0.0f, 0.0f, 0.0f);
        _idle_page->show();
    }
}

void AppClaude::onRunning()
{
    // M3 will replace this with the proper button FSM. For now we still
    // service the key manager so 'GoHome' (long-press A) doesn't lock up.
    if (_key_manager) {
        // Note: do NOT call close() — this app is the only one installed,
        // and closing leaves the user stranded with no UI. Just consume.
        (void)_key_manager->update();
    }

    // Tick stale-state pruning every second. M4 will wire actual BLE input.
    if (GetHAL().millis() - _last_tick_ms > 1000) {
        _last_tick_ms = GetHAL().millis();
        // mclog::tagInfo(getAppInfo().name, "tick");
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

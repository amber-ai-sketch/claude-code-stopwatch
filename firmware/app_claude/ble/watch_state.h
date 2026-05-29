/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * The state struct that the JSON parser fills and the UI reads. Mirror
 * of buddy's TamaState but with statusline-derived fields and a single
 * `current_tool` slot for picking which busy.* motion graphic to show.
 */
#pragma once
#include <stdint.h>
#include <string>

namespace clawd_watch {

struct WatchState {
    // Session-level
    uint8_t  sessions_total   = 0;
    uint8_t  sessions_running = 0;
    uint8_t  sessions_waiting = 0;

    // Connection liveness
    bool     connected        = false;
    uint32_t last_updated_ms  = 0;

    // Headline message ("idle (3)" / "running" / "approve: Bash" / ...)
    std::string msg;

    // Pending approval — empty id means no prompt is active.
    std::string prompt_id;
    std::string prompt_tool;
    std::string prompt_hint;

    // Statusline-derived (clawd-watch protocol extension)
    std::string model_name;
    float    cost_usd     = 0.0f;
    bool     cost_valid   = false;
    float    context_pct  = 0.0f;
    bool     context_valid = false;
    float    rate_5h_pct  = 0.0f;
    bool     rate_5h_valid = false;
    float    rate_7d_pct  = 0.0f;
    bool     rate_7d_valid = false;

    // Most recent PreToolUse tool — drives busy.bash / busy.edit / busy.web
    // motion graphic. Empty when no tool is running.
    std::string current_tool;
};

}  // namespace clawd_watch

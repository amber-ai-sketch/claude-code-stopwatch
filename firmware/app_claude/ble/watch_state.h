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
#include <vector>

namespace clawd_watch {

// One Claude Code session as described by a heartbeat's "sessions" array.
// Mirrors the compact keys in state.py::heartbeat_snapshot.
struct SessionInfo {
    std::string sid;          // first 8 chars of the session id
    bool   running     = false;
    bool   waiting     = false;  // has a pending approval
    float  context_pct = 0.0f;
    bool   context_valid = false;
    float  cost_usd    = 0.0f;
    bool   cost_valid  = false;
    std::string model;
    std::string tool;
    std::string project;      // workspace dir basename, shown as the page title
    // Live context-window token counts (current_usage). -1 = unknown.
    int32_t input_tokens        = -1;
    int32_t output_tokens       = -1;
    int32_t cache_read_tokens   = -1;
    int32_t cache_create_tokens = -1;
};

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

    // Per-session detail for the swipeable detail pages. Rebuilt on each
    // heartbeat that carries a "sessions" array.
    std::vector<SessionInfo> session_details;
};

}  // namespace clawd_watch

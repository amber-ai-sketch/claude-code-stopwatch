/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Session detail page — one tile per Claude Code session. The project
 * name is the title; a 2×2 grid of token counts (input / output / cache
 * hit / cache write) is the body. The context-window fill is the arc
 * ring around the edge (bottom-gapped so the pager and cost sit in the
 * gap, never overlapping the arc). Warm orange throughout — peace, not
 * panic.
 *
 * Layout (top → bottom):
 *   claude-code-stopwatch   (project title, hero)
 *   sonnet-4.5              (model, grey)
 *   [chip]                  (working / idle / your turn)
 *   15.5k INPUT   1.2k OUTPUT      (NumberFlow, spring-animated digits)
 *   82k CACHE HIT 5.0k CACHE WRITE (NumberFlow, spring-animated digits)
 *   $cost               (NumberFlowFloat, spring-animated)
 *   tool                (static label)
 *
 * The arc ring shows context % silently; there is no numeric "70%".
 * The pager dots (in WatchFace) show which session this is; there is no
 * "SESSION n / m" text.
 *
 * Built into a caller-provided parent tile.
 */
#pragma once
#include <lvgl.h>
#include "number_flow/number_flow.hpp"
#include "number_flow/number_flow_float.hpp"
#include <string>
#include "clawd_pet.h"  // for ClawdState

namespace clawd_watch {

class SessionPage {
public:
    explicit SessionPage(lv_obj_t* parent);

    // ordinal/count are 1-based (kept for API symmetry; not shown anymore).
    // ctx_pct < 0 means unknown. cost < 0 means unknown. token counts < 0
    // mean unknown (the cell shows "—").
    // title is the preferred display name (session_name > worktree_name >
    // agent_name); project is the fallback (workspace dir basename).
    void update(int ordinal, int count,
                ClawdState state,
                const std::string& title,
                const std::string& project,
                const std::string& model,
                float ctx_pct,
                float cost_usd,
                const std::string& tool,
                int32_t input_tokens,
                int32_t output_tokens,
                int32_t cache_read_tokens,
                int32_t cache_create_tokens);

private:
    using NumberFlow     = smooth_ui_toolkit::lvgl_cpp::NumberFlow;
    using NumberFlowFloat = smooth_ui_toolkit::lvgl_cpp::NumberFlowFloat;

    lv_obj_t* _ring        = nullptr;  // context fill ring (bottom-gapped)
    lv_obj_t* _title       = nullptr;  // project name
    lv_obj_t* _model       = nullptr;
    lv_obj_t* _chip        = nullptr;
    lv_obj_t* _chip_label  = nullptr;
    NumberFlow* _tok_val[4] = {};      // INPUT / OUTPUT / CACHE HIT / CACHE WRITE (animated)
    NumberFlowFloat* _cost_flow = nullptr;  // "$cost" (animated)
    lv_obj_t* _tool_label  = nullptr;  // "tool" name (static)
    int32_t _last_tok_scaled[4] = {-1, -1, -1, -1};  // track suffix changes
};

}  // namespace clawd_watch

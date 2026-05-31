/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Overview page — tile 0 of the swipeable watch face. Clawd the mascot
 * center stage, a status chip on top, and an "N / M sessions" tally
 * below.
 *
 * State mapping (peace + proactive, never anxious):
 *   any pending approval  → Waiting  ("your turn", calm hello)
 *   any running session    → Working ("working", steady bob)
 *   otherwise              → Idle    ("idle", sleepy breathing)
 *
 * Builds into a caller-provided parent (a tileview tile), not its own
 * screen.
 */
#pragma once
#include <lvgl.h>
#include <memory>
#include <optional>
#include "clawd_pet.h"

namespace clawd_watch {

class OverviewPage {
public:
    explicit OverviewPage(lv_obj_t* parent);
    ~OverviewPage();

    void update(int sessions_total, int sessions_running, int sessions_waiting,
                std::optional<ClawdState> override_state = std::nullopt);

private:
    lv_obj_t* _ring       = nullptr;  // soft breathing arc (working/waiting)
    lv_obj_t* _chip       = nullptr;  // rounded status pill
    lv_obj_t* _chip_label = nullptr;
    lv_obj_t* _count      = nullptr;  // "2 / 3"
    lv_obj_t* _count_sub  = nullptr;  // "running" / "idle" / ...
    std::unique_ptr<ClawdPet> _pet;

    ClawdState _shown = ClawdState::Idle;
};

}  // namespace clawd_watch

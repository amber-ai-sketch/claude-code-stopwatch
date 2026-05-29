/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Idle page — what the watch shows when connected to the daemon but no
 * tool is running. Pixel pet (bufo GIF) center stage, cost + rates as
 * peripheral readouts.
 *
 * Field source: heartbeat JSON from clawd-watch-daemon
 *   pet    → looping GIF center
 *   model  → text top
 *   cost   → number bottom
 *   r5h    → arc top-right (green→red as % climbs)
 *   r7d    → arc bottom-right
 *   ctx    → arc bottom-left (context window remaining)
 *
 * Round-screen note: the AMOLED is 466×466 physical, so widgets near the
 * corners get clipped. Center the important data; arcs and rings work
 * naturally on round.
 */
#pragma once
#include <lvgl.h>
#include <string>

namespace clawd_watch {

class IdlePage {
public:
    IdlePage();
    ~IdlePage();

    // Replace contents from a fresh heartbeat snapshot.
    void update(const std::string& model_name,
                float cost_usd,
                float context_pct,
                float rate_5h_pct,
                float rate_7d_pct);

    // Show / hide via LVGL screen load.
    void show();
    void hide();

    lv_obj_t* screen() { return _screen; }

private:
    lv_obj_t* _screen      = nullptr;
    lv_obj_t* _pet_gif     = nullptr;  // looping bufo idle animation
    lv_obj_t* _model_label = nullptr;
    lv_obj_t* _cost_label  = nullptr;
    lv_obj_t* _arc_5h      = nullptr;
    lv_obj_t* _arc_7d      = nullptr;
    lv_obj_t* _arc_ctx     = nullptr;
    lv_obj_t* _hint_label  = nullptr;  // small text "no sessions" / "idle"
};

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Clawd — the Claude Code pixel mascot, rendered as a cluster of LVGL
 * rectangles (no GIF, near-zero flash). One body block, two claws, four
 * legs, two eyes. Three calm, proactive states driven by lv_anim:
 *
 *   working : slow steady bob + gently alternating legs (busy, not frantic)
 *   idle    : slow breathing squash + sleepy eye-lines (at rest)
 *   waiting : leans toward you, lifts one claw in a calm hello (an invite)
 *
 * Tone is peace + proactive: warm Claude orange throughout, no alarm
 * colors, no jitter. See firmware/design/clawd_pet.html for the reference.
 *
 * Place it on any parent; it builds a fixed-size cluster centered on the
 * parent's coordinate system. Call set_state() to switch animations.
 */
#pragma once
#include <lvgl.h>

namespace clawd_watch {

enum class ClawdState { Working, Idle, Waiting, Celebrate };

class ClawdPet {
public:
    // Builds the mascot as a child cluster of `parent`, centered with the
    // given y offset. `unit` scales the whole creature (pixel size).
    ClawdPet(lv_obj_t* parent, int center_x, int center_y);
    ~ClawdPet();

    void set_state(ClawdState state);

    // Called by the lv_anim exec callback. `t` in [0,1] is the eased phase.
    void apply_phase(float t);

private:
    void _stop_anims();

    lv_obj_t* _root   = nullptr;  // transparent container holding all parts
    lv_obj_t* _body   = nullptr;
    lv_obj_t* _clawL  = nullptr;
    lv_obj_t* _clawR  = nullptr;
    lv_obj_t* _legs[4] = {};
    lv_obj_t* _eyeL   = nullptr;
    lv_obj_t* _eyeR   = nullptr;

    ClawdState _state = ClawdState::Idle;
};

}  // namespace clawd_watch

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
    void apply_zzz_phase(float t);
    void set_leg_phase(float t);   // leg stepping callback (1.2s cycle)

private:
    void _stop_anims();
    void _apply_legs_phase(float t);  // alternating leg step positions

    lv_obj_t* _root   = nullptr;  // transparent container (no transform → no clipping)
    lv_obj_t* _char   = nullptr;  // character sub-container (body/claws/legs/eyes)
    lv_obj_t* _body   = nullptr;
    lv_obj_t* _clawL  = nullptr;
    lv_obj_t* _clawR  = nullptr;
    lv_obj_t* _legs[4] = {};
    lv_obj_t* _eyeL   = nullptr;
    lv_obj_t* _eyeR   = nullptr;

    // zzz floating labels (idle state only) — on parent, not _root, to avoid clipping
    lv_obj_t* _zzz[3] = {};
    lv_anim_t _zzzAnim{};
    int _zzz_base_y[3] = {};     // starting y for each z (pre-animation, parent coords)
    int _root_px = 0, _root_py = 0;  // root position on parent (for zzz positioning)

    float _leg_phase = 0;        // separate leg cycle (1.2s vs body 1.8s)
    lv_anim_t _legAnim{};

    ClawdState _state = ClawdState::Idle;
};

}  // namespace clawd_watch

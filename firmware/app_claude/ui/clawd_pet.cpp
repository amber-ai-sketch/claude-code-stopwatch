/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "clawd_pet.h"
#include "design_tokens.h"

namespace clawd_watch {

namespace {

// Root-local base layout (pixels). Origin at the cluster's top-left.
// Mirrors firmware/design/clawd_pet.html, scaled 1.2× for the 466 screen.
//
// Hierarchy: _root (no transform, no clipping)
//              └─ _char (all transforms go here: idle scaleY, waiting rotate)
//                   └─ body, claws, legs, eyes
//              └─ zzz[0..2] (float above _char, outside its clip region)
//
// Keeping _root transform-free ensures it never gets its own rendering layer,
// so the zzz labels at negative y stay visible.
constexpr int kRootW = 192;
constexpr int kRootH = 166;  // extra height for zzz drift above body

// All character parts live inside _char, offset down to leave room for zzz.
constexpr int kCharY = 24;

constexpr int kBodyX = 22,  kBodyY = kCharY,  kBodyW = 148, kBodyH = 86;
constexpr int kClawW = 22;
constexpr int kClawLX = 0,   kClawRX = 170, kClawY = kCharY + 22;
constexpr int kClawYRaised = kCharY + 6;             // waiting: left claw lifted
constexpr int kLegY = kCharY + 86, kLegW = 19, kLegH = 30;
constexpr int kLegX[4] = {34, 67, 106, 139};
constexpr int kEyeW = 22, kEyeY = kCharY + 22;
constexpr int kEyeLX = 50, kEyeRX = 120;
constexpr int kEyeLineH = 6, kEyeLineY = kCharY + 34;  // idle: sleepy eye-lines

const lv_color_t kDark = lv_color_make(0x0d, 0x0d, 0x0d);

lv_obj_t* make_rect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_radius(o, 0, 0);
    return o;
}

}  // namespace

ClawdPet::ClawdPet(lv_obj_t* parent, int center_x, int center_y)
{
    _root_px = center_x - kRootW / 2;
    _root_py = center_y - kRootH / 2;
    _root = lv_obj_create(parent);
    lv_obj_remove_style_all(_root);
    lv_obj_set_size(_root, kRootW, kRootH);
    lv_obj_set_pos(_root, _root_px, _root_py);

    // Character sub-container: all body parts go here. Transforms (scaleY for
    // breathing, rotation for waiting lean) are applied to _char, never to
    // _root, so _root never creates its own rendering layer and never clips.
    _char = lv_obj_create(_root);
    lv_obj_remove_style_all(_char);
    lv_obj_set_size(_char, kRootW, kRootH);
    lv_obj_set_pos(_char, 0, 0);

    _clawL = make_rect(_char, kClawLX, kClawY, kClawW, kClawW, kOrange);
    _clawR = make_rect(_char, kClawRX, kClawY, kClawW, kClawW, kOrange);
    _body  = make_rect(_char, kBodyX, kBodyY, kBodyW, kBodyH, kOrange);
    for (int i = 0; i < 4; i++)
        _legs[i] = make_rect(_char, kLegX[i], kLegY, kLegW, kLegH, kOrange);
    _eyeL = make_rect(_char, kEyeLX, kEyeY, kEyeW, kEyeW, kDark);
    _eyeR = make_rect(_char, kEyeRX, kEyeY, kEyeW, kEyeW, kDark);

    // zzz floating labels — on _parent_ (not _root or _char).
    // Root's _char container is opaque at the body area, which would cover
    // zzz if they were children of _root. Placing them on the parent and
    // positioning in parent coords keeps them visible on top of everything.
    const lv_font_t* z_fonts[3] = {
        &lv_font_montserrat_18, &lv_font_montserrat_22, &lv_font_montserrat_26
    };
    for (int i = 0; i < 3; i++) {
        _zzz[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(_zzz[i], lv_color_make(0x6a, 0x6a, 0x6a), 0);
        lv_obj_set_style_text_font(_zzz[i], z_fonts[i], 0);
        lv_label_set_text(_zzz[i], "z");
        // Position above body top (root y + kCharY), staggered upward.
        _zzz_base_y[i] = _root_py + kCharY - 4 - i * 18;
        lv_obj_set_pos(_zzz[i], _root_px + 162 + i * 16, _zzz_base_y[i]);
        lv_obj_set_style_opa(_zzz[i], LV_OPA_TRANSP, 0);
    }

    set_state(ClawdState::Idle);
}

ClawdPet::~ClawdPet()
{
    _stop_anims();
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }
}

void ClawdPet::_stop_anims()
{
    lv_anim_delete(this, nullptr);  // stops main + zzz + leg anims (all use this as var)
}

namespace {

// Single exec callback drives the whole creature from a 0..1000 phase.
// Playback makes it oscillate 0→1000→0, so every motion eases back and
// forth — calm, never jittery. The owning ClawdPet is passed as var.
void clawd_tick(void* var, int32_t phase)
{
    static_cast<ClawdPet*>(var)->apply_phase(phase / 1000.0f);
}

void clawd_zzz_tick(void* var, int32_t phase)
{
    static_cast<ClawdPet*>(var)->apply_zzz_phase(phase / 1000.0f);
}

void clawd_leg_tick(void* var, int32_t phase)
{
    static_cast<ClawdPet*>(var)->set_leg_phase(phase / 1000.0f);
}

void clawd_celebrate_tick(void* var, int32_t /*phase*/)
{
    auto* pet = static_cast<ClawdPet*>(var);
    float t = lv_tick_elaps(pet->_celebrate_start_tick) / 1000.0f;
    pet->_celebrate_spring.next(t);
    pet->apply_phase(pet->_celebrate_spring.value);
    if (pet->_celebrate_spring.done) {
        lv_anim_del(var, clawd_celebrate_tick);
    }
}

void clawd_celebrate_ready(lv_anim_t* a)
{
    // Ensure we land exactly on the settled value.
    static_cast<ClawdPet*>(a->var)->apply_phase(1.0f);
}

}  // namespace

void ClawdPet::set_state(ClawdState state)
{
    _state = state;
    _stop_anims();

    // Reset parts to base layout before the new state animates them.
    lv_obj_set_pos(_clawL, kClawLX, (state == ClawdState::Waiting) ? kClawYRaised : kClawY);
    lv_obj_set_pos(_clawR, kClawRX, kClawY);
    lv_obj_set_pos(_body, kBodyX, kBodyY);
    lv_obj_set_size(_body, kBodyW, kBodyH);
    for (int i = 0; i < 4; i++) lv_obj_set_pos(_legs[i], kLegX[i], kLegY);
    // Reset all transforms on _char.
    lv_obj_set_pos(_char, 0, 0);
    lv_obj_set_style_transform_rotation(_char, 0, 0);
    lv_obj_set_style_transform_scale_y(_char, 256, 0);  // 1.0× = 256

    // Body / claw / leg color: warm orange when active, dimmer at rest.
    lv_color_t body_color = (state == ClawdState::Idle) ? kOrangeDim : kOrange;
    lv_obj_set_style_bg_color(_body, body_color, 0);
    lv_obj_set_style_bg_color(_clawL, body_color, 0);
    lv_obj_set_style_bg_color(_clawR, body_color, 0);
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(_legs[i], body_color, 0);

    // Eyes: dimmer dark when idle, sleepy lines when idle, square otherwise.
    lv_color_t eye_color = (state == ClawdState::Idle)
        ? lv_color_make(0x08, 0x08, 0x08) : kDark;
    lv_obj_set_style_bg_color(_eyeL, eye_color, 0);
    lv_obj_set_style_bg_color(_eyeR, eye_color, 0);
    if (state == ClawdState::Idle) {
        lv_obj_set_pos(_eyeL, kEyeLX, kEyeLineY);
        lv_obj_set_size(_eyeL, kEyeW, kEyeLineH);
        lv_obj_set_pos(_eyeR, kEyeRX, kEyeLineY);
        lv_obj_set_size(_eyeR, kEyeW, kEyeLineH);
    } else {
        lv_obj_set_pos(_eyeL, kEyeLX, kEyeY);
        lv_obj_set_size(_eyeL, kEyeW, kEyeW);
        lv_obj_set_pos(_eyeR, kEyeRX, kEyeY);
        lv_obj_set_size(_eyeR, kEyeW, kEyeW);
    }

    // Reset zzz labels (only visible in idle).
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_opa(_zzz[i], LV_OPA_TRANSP, 0);

    // Slow, eased oscillation. Durations are long on purpose — peace, not
    // urgency. waiting is the gentlest "hello", not an alarm.
    // Celebrate uses spring physics for a lively bounce with overshoot.
    if (state == ClawdState::Celebrate) {
        _celebrate_spring.setSpringOptions(1000.0f, 0.5f, 0.3f);
        _celebrate_spring.retarget(0.0f, 1.0f);
        _celebrate_spring.init();
        _celebrate_start_tick = lv_tick_get();

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, clawd_celebrate_tick);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, 2000);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_playback_duration(&a, 0);
        lv_anim_set_repeat_count(&a, 1);
        lv_anim_set_ready_cb(&a, clawd_celebrate_ready);
        lv_anim_start(&a);

        // Energetic leg wiggle — keeps moving throughout the 2s celebrate.
        _leg_phase = 0;
        lv_anim_init(&_legAnim);
        lv_anim_set_var(&_legAnim, this);
        lv_anim_set_exec_cb(&_legAnim, clawd_leg_tick);
        lv_anim_set_values(&_legAnim, 0, 1000);
        lv_anim_set_duration(&_legAnim, 400);
        lv_anim_set_path_cb(&_legAnim, lv_anim_path_ease_in_out);
        lv_anim_set_playback_duration(&_legAnim, 400);
        lv_anim_set_repeat_count(&_legAnim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&_legAnim);
    } else {
        uint32_t dur = (state == ClawdState::Working) ? 1800
                     : (state == ClawdState::Idle)    ? 4000
                                                      : 1500;  // waiting
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, clawd_tick);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    // Idle breathing: scaleY on _char with pivot at body CENTER (symmetric squash).
    if (state == ClawdState::Idle) {
        lv_obj_set_style_transform_pivot_x(_char, kBodyX + kBodyW / 2, 0);
        lv_obj_set_style_transform_pivot_y(_char, kBodyY + kBodyH / 2, 0);
    }

    // Working: separate 1.2s leg stepping (faster than 1.8s body bob).
    if (state == ClawdState::Working) {
        _leg_phase = 0;
        lv_anim_init(&_legAnim);
        lv_anim_set_var(&_legAnim, this);
        lv_anim_set_exec_cb(&_legAnim, clawd_leg_tick);
        lv_anim_set_values(&_legAnim, 0, 1000);
        lv_anim_set_duration(&_legAnim, 1200);
        lv_anim_set_path_cb(&_legAnim, lv_anim_path_ease_in_out);
        lv_anim_set_playback_duration(&_legAnim, 1200);
        lv_anim_set_repeat_count(&_legAnim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&_legAnim);
    }

    // zzz drift animation for idle — separate from the breathing cycle.
    if (state == ClawdState::Idle) {
        lv_anim_init(&_zzzAnim);
        lv_anim_set_var(&_zzzAnim, this);
        lv_anim_set_exec_cb(&_zzzAnim, clawd_zzz_tick);
        lv_anim_set_values(&_zzzAnim, 0, 1000);
        lv_anim_set_duration(&_zzzAnim, 4000);
        lv_anim_set_path_cb(&_zzzAnim, lv_anim_path_ease_in_out);
        lv_anim_set_playback_duration(&_zzzAnim, 4000);
        lv_anim_set_repeat_count(&_zzzAnim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&_zzzAnim);
    }

    // Waiting: set pivots; apply_phase() drives the actual rotation values.
    if (state == ClawdState::Waiting) {
        lv_obj_set_style_transform_pivot_x(_char, kBodyX + kBodyW / 2, 0);
        lv_obj_set_style_transform_pivot_y(_char, kBodyY + kBodyH, 0);
        // Claw wave pivots at bottom-right corner (attachment to body).
        lv_obj_set_style_transform_pivot_x(_clawL, kClawW, 0);
        lv_obj_set_style_transform_pivot_y(_clawL, kClawW, 0);
    }
}

void ClawdPet::apply_phase(float t)
{
    switch (_state) {
    case ClawdState::Working: {
        // Body bobs -4px (1.8s cycle). Legs use their own 1.2s cycle.
        int bob = (int)(-4.0f * t);
        lv_obj_set_y(_body, kBodyY + bob);
        lv_obj_set_y(_eyeL, kEyeY + bob);
        lv_obj_set_y(_eyeR, kEyeY + bob);
        lv_obj_set_y(_clawL, kClawY + bob);
        lv_obj_set_y(_clawR, kClawY + bob);
        _apply_legs_phase(_leg_phase);
        break;
    }
    case ClawdState::Idle: {
        // Breathing squash: scaleY(1.0→0.95) on _char, pivot at body center.
        // translateY(0→+2px) settles the body down as it compresses.
        lv_obj_set_style_transform_scale_y(_char, 256 - (int)(13.0f * t), 0);
        lv_obj_set_y(_char, (int)(2.0f * t));
        break;
    }
    case ClawdState::Waiting: {
        // _char leanIn: translateY(0→-3px), rotate(0→-2°). Moves all parts.
        lv_obj_set_y(_char, (int)(-3.0f * t));
        lv_obj_set_style_transform_rotation(_char, (int)(-20.0f * t), 0);
        // clawL helloWave: y from kClawYRaised-6 to kClawYRaised-10, rot -12°→-20°
        lv_obj_set_y(_clawL, kClawYRaised - 6 - (int)(4.0f * t));
        lv_obj_set_style_transform_rotation(_clawL, -120 + (int)(-80.0f * t), 0);
        break;
    }
    case ClawdState::Celebrate: {
        // Happy bounce: body jumps -12px, claws up, legs tuck.
        int jump = (int)(-12.0f * t);
        lv_obj_set_y(_body, kBodyY + jump);
        lv_obj_set_y(_eyeL, kEyeY + jump);
        lv_obj_set_y(_eyeR, kEyeY + jump);
        lv_obj_set_y(_clawL, kClawYRaised + jump);
        lv_obj_set_y(_clawR, kClawYRaised + jump);
        for (int i = 0; i < 4; i++)
            lv_obj_set_y(_legs[i], kLegY + (int)(3.0f * t));
        break;
    }
    }
}

void ClawdPet::_apply_legs_phase(float t)
{
    // Soft alternating step: pairs (0,2) and (1,3) move counter-phase.
    int step = (int)(2.0f * t);
    lv_obj_set_y(_legs[0], kLegY + step);
    lv_obj_set_y(_legs[2], kLegY + step);
    lv_obj_set_y(_legs[1], kLegY + 2 - step);
    lv_obj_set_y(_legs[3], kLegY + 2 - step);
}

void ClawdPet::set_leg_phase(float t)
{
    _leg_phase = t;
}

void ClawdPet::apply_zzz_phase(float t)
{
    // Three z's: staggered fade, upward drift -26px, scale 0.6→1.05.
    for (int i = 0; i < 3; i++) {
        // Each z starts 0.33 apart in the cycle.
        float local = t - i * 0.33f;
        if (local < 0) local += 1.0f;
        // Opacity: fade in 0→0.3, hold 0.3→0.7, fade out 0.7→1.0.
        lv_opa_t opa;
        if (local < 0.3f)
            opa = (lv_opa_t)(LV_OPA_COVER * (local / 0.3f));
        else if (local < 0.7f)
            opa = LV_OPA_COVER;
        else
            opa = (lv_opa_t)(LV_OPA_COVER * ((1.0f - local) / 0.3f));
        lv_obj_set_style_opa(_zzz[i], opa, 0);
        // Drift upward -26px, scale from 0.6 to 1.05 over the cycle.
        int dy = (int)(-26.0f * t);
        lv_obj_set_y(_zzz[i], _zzz_base_y[i] + dy);
        int sc = 154 + (int)(118.0f * t);  // 0.6×=154 → 1.05×=269
        lv_obj_set_style_transform_scale(_zzz[i], sc, 0);
        lv_obj_set_style_transform_pivot_x(_zzz[i], 0, 0);
        lv_obj_set_style_transform_pivot_y(_zzz[i], 0, 0);
    }
}

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "clawd_pet.h"

namespace clawd_watch {

namespace {

// Root-local base layout (pixels). Origin at the cluster's top-left.
// Mirrors firmware/design/clawd_pet.html, scaled for the 466 screen.
constexpr int kRootW = 192;
constexpr int kRootH = 118;

constexpr int kBodyX = 22,  kBodyY = 0,  kBodyW = 148, kBodyH = 86;
constexpr int kClawW = 22;
constexpr int kClawLX = 0,   kClawRX = 170, kClawY = 22;
constexpr int kClawYRaised = 6;             // waiting: left claw lifted
constexpr int kLegY = 86, kLegW = 19, kLegH = 30;
constexpr int kLegX[4] = {34, 67, 106, 139};
constexpr int kEyeW = 22, kEyeY = 22;
constexpr int kEyeLX = 50, kEyeRX = 120;
constexpr int kEyeLineH = 6, kEyeLineY = 34;  // idle: sleepy eye-lines

const lv_color_t kOrange    = lv_color_make(0xD9, 0x77, 0x57);
const lv_color_t kOrangeDim = lv_color_make(0x9c, 0x6b, 0x56);
const lv_color_t kDark      = lv_color_make(0x0d, 0x0d, 0x0d);

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
    _root = lv_obj_create(parent);
    lv_obj_remove_style_all(_root);
    lv_obj_set_size(_root, kRootW, kRootH);
    lv_obj_set_pos(_root, center_x - kRootW / 2, center_y - kRootH / 2);

    _clawL = make_rect(_root, kClawLX, kClawY, kClawW, kClawW, kOrange);
    _clawR = make_rect(_root, kClawRX, kClawY, kClawW, kClawW, kOrange);
    _body  = make_rect(_root, kBodyX, kBodyY, kBodyW, kBodyH, kOrange);
    for (int i = 0; i < 4; i++)
        _legs[i] = make_rect(_root, kLegX[i], kLegY, kLegW, kLegH, kOrange);
    _eyeL = make_rect(_root, kEyeLX, kEyeY, kEyeW, kEyeW, kDark);
    _eyeR = make_rect(_root, kEyeRX, kEyeY, kEyeW, kEyeW, kDark);

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
    // One anim per pet, keyed on the root. Removing by var stops it cleanly.
    lv_anim_delete(this, nullptr);
}

namespace {

// Single exec callback drives the whole creature from a 0..1000 phase.
// Playback makes it oscillate 0→1000→0, so every motion eases back and
// forth — calm, never jittery. The owning ClawdPet is passed as var.
void clawd_tick(void* var, int32_t phase)
{
    static_cast<ClawdPet*>(var)->apply_phase(phase / 1000.0f);
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

    // Body / claw color: warm orange when active, dimmer at rest.
    lv_color_t body_color = (state == ClawdState::Idle) ? kOrangeDim : kOrange;
    lv_obj_set_style_bg_color(_body, body_color, 0);
    lv_obj_set_style_bg_color(_clawL, body_color, 0);
    lv_obj_set_style_bg_color(_clawR, body_color, 0);

    // Eyes: sleepy lines when idle, square otherwise.
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

    // Slow, eased oscillation. Durations are long on purpose — peace, not
    // urgency. waiting is the gentlest "hello", not an alarm.
    uint32_t dur = (state == ClawdState::Working) ? 1800
                 : (state == ClawdState::Idle)    ? 2000
                                                  : 1500;  // waiting (one way)

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, clawd_tick);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_playback_duration(&a, dur);            // oscillate back
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void ClawdPet::apply_phase(float t)
{
    switch (_state) {
    case ClawdState::Working: {
        // Whole body bobs up a few px; legs step in gentle counter-phase.
        int bob = (int)(-5.0f * t);
        lv_obj_set_y(_body, kBodyY + bob);
        lv_obj_set_y(_eyeL, kEyeY + bob);
        lv_obj_set_y(_eyeR, kEyeY + bob);
        lv_obj_set_y(_clawL, kClawY + bob);
        lv_obj_set_y(_clawR, kClawY + bob);
        // legs 0,2 down as 1,3 up — a soft scuttle, not a sprint.
        int step = (int)(3.0f * t);
        lv_obj_set_y(_legs[0], kLegY + step);
        lv_obj_set_y(_legs[2], kLegY + step);
        lv_obj_set_y(_legs[1], kLegY + (3 - step));
        lv_obj_set_y(_legs[3], kLegY + (3 - step));
        break;
    }
    case ClawdState::Idle: {
        // Breathing squash: body shrinks in height and settles down a touch.
        int squash = (int)(5.0f * t);
        lv_obj_set_size(_body, kBodyW, kBodyH - squash);
        lv_obj_set_y(_body, kBodyY + squash);
        lv_obj_set_y(_eyeL, kEyeLineY + squash);
        lv_obj_set_y(_eyeR, kEyeLineY + squash);
        break;
    }
    case ClawdState::Waiting: {
        // Leans toward the viewer (whole creature lifts slightly) and the
        // raised left claw waves a small, calm hello.
        int lift = (int)(-4.0f * t);
        lv_obj_set_y(_body, kBodyY + lift);
        lv_obj_set_y(_eyeL, kEyeY + lift);
        lv_obj_set_y(_eyeR, kEyeY + lift);
        lv_obj_set_y(_clawR, kClawY + lift);
        lv_obj_set_y(_clawL, kClawYRaised + lift + (int)(-4.0f * t));
        break;
    }
    }
}

}  // namespace clawd_watch

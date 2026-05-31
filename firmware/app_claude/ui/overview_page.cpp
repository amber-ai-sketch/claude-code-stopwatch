/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "overview_page.h"
#include "design_tokens.h"
#include <stdio.h>

namespace clawd_watch {

namespace {

constexpr int kScreen = 466;
constexpr int kCx = kScreen / 2;
constexpr int kCy = kScreen / 2;

}  // namespace

OverviewPage::OverviewPage(lv_obj_t* parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Status chip (rounded pill) near the top.
    _chip = lv_obj_create(parent);
    lv_obj_remove_style_all(_chip);
    lv_obj_set_style_radius(_chip, 20, 0);
    lv_obj_set_style_bg_opa(_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(_chip, 18, 0);
    lv_obj_set_style_pad_ver(_chip, 7, 0);
    lv_obj_set_size(_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(_chip, LV_ALIGN_TOP_MID, 0, 88);

    _chip_label = lv_label_create(_chip);
    lv_obj_set_style_text_font(_chip_label, &lv_font_montserrat_22, 0);
    lv_label_set_text(_chip_label, "idle");
    lv_obj_center(_chip_label);

    // Clawd center, nudged up to leave room for the count below.
    _pet = std::make_unique<ClawdPet>(parent, kCx, kCy - 21);

    // Session tally — hero count, large.
    _count = lv_label_create(parent);
    lv_obj_set_style_text_font(_count, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_count, lv_color_white(), 0);
    lv_label_set_text(_count, "0 / 0");
    lv_obj_align(_count, LV_ALIGN_CENTER, 0, 118);

    _count_sub = lv_label_create(parent);
    lv_obj_set_style_text_font(_count_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_count_sub, kGrey, 0);
    lv_label_set_text(_count_sub, "no sessions");
    lv_obj_align(_count_sub, LV_ALIGN_CENTER, 0, 166);

    update(0, 0, 0);
}

OverviewPage::~OverviewPage()
{
    _pet.reset();
}

void OverviewPage::update(int sessions_total, int sessions_running, int sessions_waiting,
                          std::optional<ClawdState> override_state)
{
    // Decide state. Override takes priority (e.g. Celebrate), then
    // waiting > working > idle.
    ClawdState state = override_state.value_or(
        sessions_waiting > 0 ? ClawdState::Waiting
      : sessions_running > 0 ? ClawdState::Working
                             : ClawdState::Idle);

    // Chip text + color.
    const char* chip_text;
    lv_color_t chip_bg, chip_fg;
    if (state == ClawdState::Waiting) {
        chip_text = "your turn"; chip_fg = kAmber;
        chip_bg = kChipBgWaiting;
    } else if (state == ClawdState::Working) {
        chip_text = "working"; chip_fg = kOrange;
        chip_bg = kChipBgWorking;
    } else if (state == ClawdState::Celebrate) {
        chip_text = "done!"; chip_fg = lv_color_make(0x88, 0xDD, 0x66);
        chip_bg = lv_color_make(0x22, 0x33, 0x11);
    } else {
        chip_text = "idle"; chip_fg = kDimIdle;
        chip_bg = lv_color_make(0x22, 0x22, 0x22);
    }
    lv_label_set_text(_chip_label, chip_text);
    lv_obj_set_style_text_color(_chip_label, chip_fg, 0);
    lv_obj_set_style_bg_color(_chip, chip_bg, 0);

    // Count + subtitle. When waiting, show the waiting count prominently
    // so the user knows how many sessions need attention.
    char buf[24];
    if (sessions_waiting > 0) {
        snprintf(buf, sizeof(buf), "%d", sessions_waiting);
        lv_label_set_text(_count, buf);
        lv_label_set_text(_count_sub,
            sessions_waiting == 1 ? "needs you" : "need you");
    } else {
        snprintf(buf, sizeof(buf), "%d / %d", sessions_running, sessions_total);
        lv_label_set_text(_count, buf);
        lv_label_set_text(_count_sub,
            sessions_running > 0 ? "running"
          : sessions_total > 0   ? "idle"
                                 : "no sessions");
    }

    // Only restart the pet animation when the state actually changes —
    // re-seeding every 200ms would reset its phase and look jittery.
    if (state != _shown) {
        _pet->set_state(state);
        _shown = state;
    }
}

}  // namespace clawd_watch

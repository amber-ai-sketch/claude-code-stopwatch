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

// Battery gauge geometry.
constexpr int kBatW    = 110;   // track width
constexpr int kBatH    = 5;     // track height (thin)
constexpr int kBatY    = 44;    // vertical center of the bar

const lv_color_t kBatTrackColor = lv_color_make(0x2a, 0x2a, 0x2a);
const lv_color_t kBatFillNorm   = lv_color_make(0xc4, 0x9a, 0x6c);  // warm muted amber
const lv_color_t kBatFillLow    = lv_color_make(0x88, 0x66, 0x44);  // dim when low
const lv_color_t kBatFillChg    = lv_color_make(0x88, 0xcc, 0x66);  // warm green — charging

}  // namespace

OverviewPage::OverviewPage(lv_obj_t* parent)
{
    _root = parent;
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

    // ── Battery gauge ──
    // Thin rounded bar near the top edge, centered horizontally.
    _bat_track = lv_obj_create(parent);
    lv_obj_remove_style_all(_bat_track);
    lv_obj_set_size(_bat_track, kBatW, kBatH);
    lv_obj_set_style_radius(_bat_track, kBatH / 2, 0);
    lv_obj_set_style_bg_color(_bat_track, kBatTrackColor, 0);
    lv_obj_set_style_bg_opa(_bat_track, LV_OPA_COVER, 0);
    lv_obj_align(_bat_track, LV_ALIGN_TOP_MID, 0, kBatY - kBatH / 2);

    _bat_fill = lv_obj_create(_bat_track);
    lv_obj_remove_style_all(_bat_fill);
    lv_obj_set_height(_bat_fill, kBatH);
    lv_obj_set_style_radius(_bat_fill, kBatH / 2, 0);
    lv_obj_set_style_bg_color(_bat_fill, kBatFillNorm, 0);
    lv_obj_set_style_bg_opa(_bat_fill, LV_OPA_COVER, 0);
    lv_obj_align(_bat_fill, LV_ALIGN_LEFT_MID, 0, 0);

    _bat_pct = lv_label_create(parent);
    lv_obj_set_style_text_font(_bat_pct, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_bat_pct, kDimIdle, 0);
    lv_label_set_text(_bat_pct, "");
    // Place just to the right of the track.
    lv_obj_align_to(_bat_pct, _bat_track, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    // Clawd center — visual center of body at screen center.
    _pet = std::make_unique<ClawdPet>(parent, kCx, kCy + 4);

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
                          std::optional<ClawdState> override_state,
                          bool ble_connected,
                          uint8_t battery_pct, bool battery_charging)
{
    // ── Battery gauge (always, regardless of BLE) ──
    int fill_w = (int)battery_pct * kBatW / 100;
    if (fill_w < 2) fill_w = 2;  // min visible sliver
    lv_obj_set_width(_bat_fill, fill_w);

    lv_color_t bat_color;
    if (battery_charging) {
        bat_color = kBatFillChg;
    } else if (battery_pct <= 15) {
        bat_color = kBatFillLow;
    } else {
        bat_color = kBatFillNorm;
    }
    lv_obj_set_style_bg_color(_bat_fill, bat_color, 0);

    // Pct label — hide when charging (the green bar is self-explanatory).
    if (battery_charging) {
        lv_label_set_text(_bat_pct, "");
    } else {
        char bat_buf[8];
        snprintf(bat_buf, sizeof(bat_buf), "%d", battery_pct);
        lv_label_set_text(_bat_pct, bat_buf);
    }

    // BLE disconnected: show reconnecting state, override everything.
    if (!ble_connected) {
        lv_label_set_text(_chip_label, "reconnecting");
        lv_obj_set_style_text_color(_chip_label, kDimIdle, 0);
        lv_obj_set_style_bg_color(_chip, kChipBgIdle, 0);
        lv_label_set_text(_count, "—");
        lv_label_set_text(_count_sub, "searching");
        if (_shown != ClawdState::Idle) {
            _pet->set_state(ClawdState::Idle);
            _shown = ClawdState::Idle;
        }
        return;
    }

    // Decide state. Override takes priority (e.g. Celebrate), then
    // waiting > working > idle.  Waiting is debounced: only activate
    // after 2 consecutive "waiting" updates to avoid brief flicker.
    ClawdState raw = override_state.value_or(
        sessions_waiting > 0 ? ClawdState::Waiting
      : sessions_running > 0 ? ClawdState::Working
                             : ClawdState::Idle);

    if (raw == ClawdState::Waiting) {
        _waiting_ticks++;
    } else {
        _waiting_ticks = 0;
    }

    // During debounce (ticks < 2), fall back to Working/Idle instead of
    // Waiting — prevents a brief flicker when the daemon toggles states.
    ClawdState state;
    if (raw == ClawdState::Waiting && _waiting_ticks < 2) {
        state = sessions_running > 0 ? ClawdState::Working : ClawdState::Idle;
    } else {
        state = raw;
    }

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
    } else {
        snprintf(buf, sizeof(buf), "%d / %d", sessions_running, sessions_total);
        lv_label_set_text(_count, buf);
    }
    // Only show subtitle when there's no session at all — the chip already
    // shows the state ("working" / "idle" / "your turn") so a redundant
    // "running" / "idle" adds noise.
    lv_label_set_text(_count_sub, sessions_total > 0 ? "" : "no sessions");

    // Count text: dimmer when idle to match the muted pet.
    lv_color_t count_color = (state == ClawdState::Idle)
        ? lv_color_make(0x66, 0x66, 0x66) : lv_color_white();
    lv_obj_set_style_text_color(_count, count_color, 0);

    // Only restart the pet animation when the state actually changes —
    // re-seeding every 200ms would reset its phase and look jittery.
    if (state != _shown) {
        _pet->set_state(state);
        _shown = state;
    }
}

}  // namespace clawd_watch

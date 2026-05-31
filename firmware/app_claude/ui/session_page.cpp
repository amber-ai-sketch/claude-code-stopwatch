/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "session_page.h"
#include "design_tokens.h"
#include <stdio.h>

namespace clawd_watch {

namespace {

constexpr int kScreen = 466;

// Static caption under each token value, in grid order.
const char* kTokLabels[4] = {"INPUT", "OUTPUT", "CACHE HIT", "CACHE WRITE"};

// Two columns (centers ±76 of mid) × two rows. Y is the value's center
// offset from screen center; the caption sits 26px below.
constexpr int kColDx   = 76;
constexpr int kRow1Dy  = 6;    // value center offset from screen center
constexpr int kRow2Dy  = 74;
constexpr int kCapGap  = 26;   // caption below its value

// "15500" → "15.5k", "82000" → "82k", "950" → "950". Keeps the grid tidy.
void fmt_tokens(char* buf, size_t n, int32_t v)
{
    if (v < 0)            snprintf(buf, n, "—");
    else if (v < 1000)    snprintf(buf, n, "%d", (int)v);
    else if (v < 10000)   snprintf(buf, n, "%.1fk", v / 1000.0f);
    else                  snprintf(buf, n, "%dk", (int)(v / 1000.0f + 0.5f));
}

}  // namespace

SessionPage::SessionPage(lv_obj_t* parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Context fill ring with a 100° gap centered at the bottom, so the
    // footer + pager sit in the gap and never overlap the arc. LVGL angles
    // run clockwise from 3 o'clock; bottom is 90°. A gap of 100° spans
    // 40°→140°, so the track is the 260° arc from 140° round to 40°.
    _ring = lv_arc_create(parent);
    lv_obj_set_size(_ring, 430, 430);
    lv_obj_center(_ring);
    lv_arc_set_bg_angles(_ring, 140, 360 + 40);   // 140° → 400° (=40°), 260° track
    lv_arc_set_range(_ring, 0, 100);
    lv_arc_set_value(_ring, 0);
    lv_obj_remove_style(_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_ring, kArcTrack, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, kOrange, LV_PART_INDICATOR);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);

    // Title: project name.
    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_title, lv_color_white(), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_title, 320);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_title, "—");
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 84);

    // Model.
    _model = lv_label_create(parent);
    lv_obj_set_style_text_font(_model, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_model, kGrey, 0);
    lv_label_set_text(_model, "—");
    lv_obj_align(_model, LV_ALIGN_TOP_MID, 0, 118);

    // Status chip.
    _chip = lv_obj_create(parent);
    lv_obj_remove_style_all(_chip);
    lv_obj_set_style_radius(_chip, 16, 0);
    lv_obj_set_style_bg_opa(_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(_chip, 14, 0);
    lv_obj_set_style_pad_ver(_chip, 5, 0);
    lv_obj_set_size(_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(_chip, LV_ALIGN_TOP_MID, 0, 146);
    _chip_label = lv_label_create(_chip);
    lv_obj_set_style_text_font(_chip_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(_chip_label, "idle");
    lv_obj_center(_chip_label);

    // Token 2×2 grid: value (white, 24) over a static caption (dim, 14).
    for (int i = 0; i < 4; i++) {
        int dx = (i % 2 == 0) ? -kColDx : kColDx;
        int vy = (i < 2) ? kRow1Dy : kRow2Dy;

        _tok_val[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(_tok_val[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(_tok_val[i], lv_color_white(), 0);
        lv_label_set_text(_tok_val[i], "—");
        lv_obj_align(_tok_val[i], LV_ALIGN_CENTER, dx, vy);

        lv_obj_t* cap = lv_label_create(parent);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cap, kDim, 0);
        lv_label_set_text(cap, kTokLabels[i]);
        lv_obj_align(cap, LV_ALIGN_CENTER, dx, vy + kCapGap);
    }

    // Footer: "$cost · tool", in the ring's bottom gap.
    _footer = lv_label_create(parent);
    lv_obj_set_style_text_font(_footer, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_footer, kGrey, 0);
    lv_label_set_text(_footer, "");
    lv_obj_align(_footer, LV_ALIGN_BOTTOM_MID, 0, -64);
}

void SessionPage::update(int ordinal, int count,
                         ClawdState state,
                         const std::string& project,
                         const std::string& model,
                         float ctx_pct,
                         float cost_usd,
                         const std::string& tool,
                         int32_t input_tokens,
                         int32_t output_tokens,
                         int32_t cache_read_tokens,
                         int32_t cache_create_tokens)
{
    (void)ordinal;
    (void)count;
    char buf[64];

    lv_label_set_text(_title, project.empty() ? "session" : project.c_str());
    lv_label_set_text(_model, model.empty() ? "—" : model.c_str());

    // Chip.
    const char* chip_text;
    lv_color_t chip_bg, chip_fg;
    if (state == ClawdState::Waiting) {
        chip_text = "your turn"; chip_fg = kAmber;
        chip_bg = kChipBgWaiting;
    } else if (state == ClawdState::Working) {
        chip_text = "working"; chip_fg = kOrange;
        chip_bg = kChipBgWorking;
    } else {
        chip_text = "idle"; chip_fg = kDimIdle;
        chip_bg = lv_color_make(0x22, 0x22, 0x22);
    }
    lv_label_set_text(_chip_label, chip_text);
    lv_obj_set_style_text_color(_chip_label, chip_fg, 0);
    lv_obj_set_style_bg_color(_chip, chip_bg, 0);

    // Context ring (silent — no numeric readout).
    // When context is unknown (negative), dim the ring to distinguish
    // "no data" from "0% full".
    if (ctx_pct >= 0.0f) {
        lv_arc_set_value(_ring, (int32_t)ctx_pct);
        lv_obj_set_style_arc_color(_ring, kOrange, LV_PART_INDICATOR);
    } else {
        lv_arc_set_value(_ring, 0);
        lv_obj_set_style_arc_color(_ring, kArcUnknown, LV_PART_INDICATOR);
    }

    // Token grid.
    const int32_t toks[4] = {input_tokens, output_tokens,
                             cache_read_tokens, cache_create_tokens};
    for (int i = 0; i < 4; i++) {
        fmt_tokens(buf, sizeof(buf), toks[i]);
        lv_label_set_text(_tok_val[i], buf);
    }

    // Footer: "$cost · tool", dropping whichever is unknown/empty.
    char cost_part[24] = {0};
    if (cost_usd >= 0.0f) snprintf(cost_part, sizeof(cost_part), "$%.2f", cost_usd);
    if (cost_part[0] && !tool.empty())
        snprintf(buf, sizeof(buf), "%s · %s", cost_part, tool.c_str());
    else if (cost_part[0])
        snprintf(buf, sizeof(buf), "%s", cost_part);
    else if (!tool.empty())
        snprintf(buf, sizeof(buf), "%s", tool.c_str());
    else
        buf[0] = '\0';
    lv_label_set_text(_footer, buf);
}

}  // namespace clawd_watch

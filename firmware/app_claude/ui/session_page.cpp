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
// Equal 30px gaps: chip-bottom(174) → row1 top → row1 cap bottom → row2 top → row2 cap bottom → footer-top(364)
constexpr int kRow1Dy  = -14;  // value center offset from screen center
constexpr int kRow2Dy  =  66;
constexpr int kCapGap  = 26;   // caption below its value


}  // namespace

SessionPage::SessionPage(lv_obj_t* parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Context fill ring with a 110° gap centered at the bottom, so the
    // footer + pager sit in the gap and never overlap the arc. LVGL angles
    // run clockwise from 3 o'clock; bottom is 90°. A gap of 110° spans
    // 35°→145°, so the track is the 250° arc from 145° round to 35°.
    _ring = lv_arc_create(parent);
    lv_obj_set_size(_ring, 464, 464);
    lv_obj_center(_ring);
    lv_arc_set_bg_angles(_ring, 145, 360 + 35);   // 145° → 395° (=35°), 250° track
    lv_arc_set_range(_ring, 0, 100);
    lv_arc_set_value(_ring, 0);
    lv_obj_remove_style(_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(_ring, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_ring, kArcTrack, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, kOrange, LV_PART_INDICATOR);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    // Title: project name.
    _title = lv_label_create(parent);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_title, lv_color_white(), 0);
    lv_label_set_long_mode(_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_title, 260);
    lv_obj_set_style_text_align(_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_title, "-");
    lv_obj_align(_title, LV_ALIGN_TOP_MID, 0, 84);

    // Model — sits above the footer in the ring's bottom gap.
    _model = lv_label_create(parent);
    lv_obj_set_style_text_font(_model, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_model, kGrey, 0);
    lv_label_set_text(_model, "-");
    lv_obj_align(_model, LV_ALIGN_BOTTOM_MID, 0, -52);

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

    // Token 2×2 grid: animated NumberFlow value over a static caption.
    for (int i = 0; i < 4; i++) {
        int dx = (i % 2 == 0) ? -kColDx : kColDx;
        int vy = (i < 2) ? kRow1Dy : kRow2Dy;

        _tok_val[i] = new NumberFlow(parent);
        lv_obj_set_style_text_font(_tok_val[i]->raw_ptr(), &lv_font_montserrat_24, 0);
        _tok_val[i]->setTextColor(lv_color_white());
        lv_obj_remove_flag(_tok_val[i]->raw_ptr(), LV_OBJ_FLAG_CLICKABLE);
        _tok_val[i]->init();
        _tok_val[i]->setValue(0);
        lv_obj_align(_tok_val[i]->raw_ptr(), LV_ALIGN_CENTER, dx, vy);

        lv_obj_t* cap = lv_label_create(parent);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cap, kDim, 0);
        lv_label_set_text(cap, kTokLabels[i]);
        lv_obj_align(cap, LV_ALIGN_CENTER, dx, vy + kCapGap);
    }

    // Cost: animated "$X.XX" via NumberFlowFloat.
    // Stacked from bottom up: pager(y=36) → model → tool → cost, all
    // inside the ring's 110° bottom gap.
    _cost_flow = new NumberFlowFloat(parent);
    lv_obj_set_style_text_font(_cost_flow->raw_ptr(), &lv_font_montserrat_16, 0);
    _cost_flow->setDigitColor(lv_color_white());
    _cost_flow->setPrefixColor(kGrey);
    lv_obj_remove_flag(_cost_flow->raw_ptr(), LV_OBJ_FLAG_CLICKABLE);
    _cost_flow->setPrefix("$");
    _cost_flow->init();
    lv_obj_align(_cost_flow->raw_ptr(), LV_ALIGN_BOTTOM_MID, 0, -88);

    // Tool name: static label below the cost.
    _tool_label = lv_label_create(parent);
    lv_obj_set_style_text_font(_tool_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_tool_label, kDim, 0);
    lv_label_set_text(_tool_label, "");
    lv_obj_align(_tool_label, LV_ALIGN_BOTTOM_MID, 0, -72);

    // Model label: between tool name and pager dots (pager at y=36).
    lv_obj_align(_model, LV_ALIGN_BOTTOM_MID, 0, -56);
}

void SessionPage::update(int ordinal, int count,
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
                         int32_t cache_create_tokens)
{
    (void)ordinal;
    (void)count;

    // Display title priority: title (session_name/worktree/agent) > project > "session".
    const char* display_title = !title.empty() ? title.c_str()
                                : !project.empty() ? project.c_str()
                                : "session";
    lv_label_set_text(_title, display_title);
    lv_label_set_text(_model, model.empty() ? "-" : model.c_str());

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
    // Dim when idle or context unknown — ring only draws attention when
    // the session is active and context data is available.
    bool ring_active = (ctx_pct >= 0.0f) && (state != ClawdState::Idle);
    if (ring_active) {
        lv_arc_set_value(_ring, (int32_t)ctx_pct);
        lv_obj_set_style_arc_color(_ring, kOrange, LV_PART_INDICATOR);
    } else {
        lv_arc_set_value(_ring, 0);
        lv_obj_set_style_arc_color(_ring, kArcUnknown, LV_PART_INDICATOR);
    }

    // Token grid — NumberFlow with k-scaling for large values.
    const int32_t toks[4] = {input_tokens, output_tokens,
                             cache_read_tokens, cache_create_tokens};
    for (int i = 0; i < 4; i++) {
        int32_t scaled;
        const char* suffix;
        if (toks[i] < 0) {
            scaled = 0; suffix = "";
        } else if (toks[i] < 1000) {
            scaled = toks[i]; suffix = "";
        } else {
            scaled = (toks[i] + 500) / 1000; suffix = "k";
        }
        _tok_val[i]->setValue(scaled);
        if (_last_tok_scaled[i] != scaled) {
            _tok_val[i]->setSuffix(suffix);
            _last_tok_scaled[i] = scaled;
        }
        _tok_val[i]->update();
    }

    // Cost — NumberFlowFloat for the animated dollar amount.
    if (cost_usd >= 0.0f) {
        _cost_flow->setValue(cost_usd);
        lv_obj_clear_flag(_cost_flow->raw_ptr(), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_cost_flow->raw_ptr(), LV_OBJ_FLAG_HIDDEN);
    }
    _cost_flow->update();

    // Tool name (static label).
    if (!tool.empty()) {
        lv_label_set_text(_tool_label, tool.c_str());
        lv_obj_clear_flag(_tool_label, LV_OBJ_FLAG_HIDDEN);
    } else if (cost_usd < 0.0f) {
        lv_obj_add_flag(_tool_label, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace clawd_watch

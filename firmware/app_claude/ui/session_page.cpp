/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "session_page.h"
#include <stdio.h>

namespace clawd_watch {

namespace {

const lv_color_t kOrange = lv_color_make(0xD9, 0x77, 0x57);
const lv_color_t kGrey   = lv_color_make(0x8a, 0x8a, 0x8a);
const lv_color_t kDim    = lv_color_make(0x5e, 0x5e, 0x5e);

}  // namespace

SessionPage::SessionPage(lv_obj_t* parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // Context fill ring.
    _ring = lv_arc_create(parent);
    lv_obj_set_size(_ring, 396, 396);
    lv_obj_center(_ring);
    lv_arc_set_rotation(_ring, 270);
    lv_arc_set_bg_angles(_ring, 0, 360);
    lv_arc_set_range(_ring, 0, 100);
    lv_arc_set_value(_ring, 0);
    lv_obj_remove_style(_ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_ring, lv_color_make(0x1b, 0x1b, 0x1b), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_ring, kOrange, LV_PART_INDICATOR);
    lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);

    _ordinal = lv_label_create(parent);
    lv_obj_set_style_text_font(_ordinal, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_ordinal, kDim, 0);
    lv_label_set_text(_ordinal, "SESSION 1 / 1");
    lv_obj_align(_ordinal, LV_ALIGN_TOP_MID, 0, 70);

    _chip = lv_obj_create(parent);
    lv_obj_remove_style_all(_chip);
    lv_obj_set_style_radius(_chip, 16, 0);
    lv_obj_set_style_bg_opa(_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(_chip, 14, 0);
    lv_obj_set_style_pad_ver(_chip, 5, 0);
    lv_obj_set_size(_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(_chip, LV_ALIGN_TOP_MID, 0, 100);
    _chip_label = lv_label_create(_chip);
    lv_obj_set_style_text_font(_chip_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(_chip_label, "idle");
    lv_obj_center(_chip_label);

    _model = lv_label_create(parent);
    lv_obj_set_style_text_font(_model, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_model, kGrey, 0);
    lv_label_set_text(_model, "—");
    lv_obj_align(_model, LV_ALIGN_TOP_MID, 0, 150);

    _pct = lv_label_create(parent);
    lv_obj_set_style_text_font(_pct, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_pct, lv_color_white(), 0);
    lv_label_set_text(_pct, "—");
    lv_obj_align(_pct, LV_ALIGN_CENTER, 0, -2);

    _pct_caption = lv_label_create(parent);
    lv_obj_set_style_text_font(_pct_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_pct_caption, kDim, 0);
    lv_label_set_text(_pct_caption, "CONTEXT USED");
    lv_obj_align(_pct_caption, LV_ALIGN_CENTER, 0, 40);

    _footer = lv_label_create(parent);
    lv_obj_set_style_text_font(_footer, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_footer, kGrey, 0);
    lv_label_set_text(_footer, "");
    lv_obj_align(_footer, LV_ALIGN_BOTTOM_MID, 0, -86);
}

void SessionPage::update(int ordinal, int count,
                         ClawdState state,
                         const std::string& model,
                         float ctx_pct,
                         float cost_usd,
                         const std::string& tool)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "SESSION %d / %d", ordinal, count);
    lv_label_set_text(_ordinal, buf);

    // Chip.
    const char* chip_text;
    lv_color_t chip_bg, chip_fg;
    if (state == ClawdState::Waiting) {
        chip_text = "your turn"; chip_fg = kOrange;
        chip_bg = lv_color_make(0x33, 0x22, 0x1a);
    } else if (state == ClawdState::Working) {
        chip_text = "working"; chip_fg = kOrange;
        chip_bg = lv_color_make(0x33, 0x22, 0x1a);
    } else {
        chip_text = "idle"; chip_fg = kGrey;
        chip_bg = lv_color_make(0x22, 0x22, 0x22);
    }
    lv_label_set_text(_chip_label, chip_text);
    lv_obj_set_style_text_color(_chip_label, chip_fg, 0);
    lv_obj_set_style_bg_color(_chip, chip_bg, 0);

    lv_label_set_text(_model, model.empty() ? "—" : model.c_str());

    if (ctx_pct >= 0.0f) {
        snprintf(buf, sizeof(buf), "%d%%", (int)(ctx_pct + 0.5f));
        lv_label_set_text(_pct, buf);
        lv_arc_set_value(_ring, (int32_t)ctx_pct);
    } else {
        lv_label_set_text(_pct, "—");
        lv_arc_set_value(_ring, 0);
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

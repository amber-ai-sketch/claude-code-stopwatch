/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "idle_page.h"
#include <hal/hal.h>
#include <stdio.h>

namespace clawd_watch {

namespace {

// 466x466 round AMOLED. Keep meaningful content in a 380px circle to
// avoid bezel clip on the curved edge.
constexpr int kScreenSize  = 466;
constexpr int kArcSize     = 80;
constexpr int kArcOffset   = 30;  // distance from screen edge
constexpr int kCenterX     = kScreenSize / 2;
constexpr int kCenterY     = kScreenSize / 2;

void make_rate_arc(lv_obj_t* parent, lv_obj_t** out_arc, int x, int y, lv_color_t color)
{
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, kArcSize, kArcSize);
    lv_obj_set_pos(arc, x, y);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    *out_arc = arc;
}

}  // namespace

IdlePage::IdlePage()
{
    // Create our own LVGL screen so show()/hide() are cheap.
    _screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_screen, lv_color_black(), 0);

    // Model name top.
    _model_label = lv_label_create(_screen);
    lv_label_set_text(_model_label, "—");
    lv_obj_set_style_text_color(_model_label, lv_color_white(), 0);
    lv_obj_align(_model_label, LV_ALIGN_TOP_MID, 0, 80);

    // Cost (big) center.
    _cost_label = lv_label_create(_screen);
    lv_label_set_text(_cost_label, "$0.00");
    lv_obj_set_style_text_color(_cost_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(_cost_label, &lv_font_montserrat_48, 0);
    lv_obj_align(_cost_label, LV_ALIGN_CENTER, 0, 0);

    // Hint underneath cost.
    _hint_label = lv_label_create(_screen);
    lv_label_set_text(_hint_label, "no sessions");
    lv_obj_set_style_text_color(_hint_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(_hint_label, LV_ALIGN_CENTER, 0, 50);

    // Three arcs: 5h, 7d, ctx.
    make_rate_arc(_screen, &_arc_5h,
                  kScreenSize - kArcSize - kArcOffset, kArcOffset,
                  lv_color_make(0x4a, 0xc8, 0x80));
    make_rate_arc(_screen, &_arc_7d,
                  kScreenSize - kArcSize - kArcOffset, kScreenSize - kArcSize - kArcOffset,
                  lv_color_make(0xff, 0xa0, 0x40));
    make_rate_arc(_screen, &_arc_ctx,
                  kArcOffset, kScreenSize - kArcSize - kArcOffset,
                  lv_color_make(0x40, 0x80, 0xff));
}

IdlePage::~IdlePage()
{
    if (_screen) {
        lv_obj_del(_screen);
        _screen = nullptr;
    }
}

void IdlePage::update(const std::string& model_name,
                      float cost_usd,
                      float context_pct,
                      float rate_5h_pct,
                      float rate_7d_pct)
{
    if (!_model_label || !_cost_label) return;

    lv_label_set_text(_model_label, model_name.empty() ? "—" : model_name.c_str());

    char buf[32];
    snprintf(buf, sizeof(buf), "$%.2f", cost_usd);
    lv_label_set_text(_cost_label, buf);

    // 0–100 → arc value. lv_arc_set_value clamps to its range (default 0–100).
    lv_arc_set_value(_arc_5h, (int32_t)rate_5h_pct);
    lv_arc_set_value(_arc_7d, (int32_t)rate_7d_pct);
    lv_arc_set_value(_arc_ctx, (int32_t)context_pct);

    lv_label_set_text(_hint_label, "idle");
}

void IdlePage::show()
{
    if (_screen) lv_screen_load(_screen);
}

void IdlePage::hide()
{
    // No-op: parent app loads a different screen when transitioning out.
}

}  // namespace clawd_watch

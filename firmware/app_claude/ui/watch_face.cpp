/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "watch_face.h"
#include <stdio.h>

namespace clawd_watch {

namespace {

const lv_color_t kOrange   = lv_color_make(0xD9, 0x77, 0x57);
const lv_color_t kDotOff   = lv_color_make(0x3a, 0x3a, 0x3a);
constexpr int kDot   = 7;    // dot diameter
constexpr int kDotGap = 9;   // center-to-center step
constexpr int kPagerY = 36;  // distance from bottom

}  // namespace

WatchFace::WatchFace()
{
    _screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(_screen);
    lv_obj_set_size(_screen, 466, 466);
    lv_obj_set_style_bg_color(_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);

    _tileview = lv_tileview_create(_screen);
    lv_obj_set_size(_tileview, 466, 466);
    lv_obj_set_style_bg_opa(_tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(_tileview, _on_tile_change, LV_EVENT_VALUE_CHANGED, this);

    // Tile 0: overview, always present.
    lv_obj_t* tile0 = lv_tileview_add_tile(_tileview, 0, 0, LV_DIR_HOR);
    _overview = std::make_unique<OverviewPage>(tile0);

    // Pager overlay on top of the tileview, never scrolls.
    _pager = lv_obj_create(_screen);
    lv_obj_remove_style_all(_pager);
    lv_obj_set_size(_pager, 466, kDot * 2);
    lv_obj_align(_pager, LV_ALIGN_BOTTOM_MID, 0, -kPagerY);
    lv_obj_set_flex_flow(_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_pager, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(_pager, kDotGap - kDot, 0);

    _rebuild_pager(1);
    _highlight_pager(0);
}

WatchFace::~WatchFace()
{
    _sessions.clear();
    _overview.reset();
    if (_screen) {
        lv_obj_del(_screen);
        _screen = nullptr;
    }
}

void WatchFace::show()
{
    if (_screen) lv_screen_load(_screen);
}

void WatchFace::_rebuild_pager(int page_count)
{
    for (lv_obj_t* d : _dots) lv_obj_del(d);
    _dots.clear();
    for (int i = 0; i < page_count; i++) {
        lv_obj_t* d = lv_obj_create(_pager);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, kDot, kDot);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, kDotOff, 0);
        _dots.push_back(d);
    }
}

void WatchFace::_highlight_pager(int active)
{
    for (size_t i = 0; i < _dots.size(); i++) {
        bool cur = ((int)i == active);
        lv_obj_set_style_bg_color(_dots[i], cur ? kOrange : kDotOff, 0);
        // Active dot stretches into a short bar.
        lv_obj_set_width(_dots[i], cur ? 16 : kDot);
        lv_obj_set_style_radius(_dots[i], cur ? 3 : LV_RADIUS_CIRCLE, 0);
    }
}

void WatchFace::_rebuild_session_tiles(int count)
{
    // Drop old session pages + their tiles.
    _sessions.clear();
    for (lv_obj_t* t : _session_tiles) lv_obj_del(t);
    _session_tiles.clear();

    for (int i = 0; i < count; i++) {
        lv_obj_t* tile = lv_tileview_add_tile(_tileview, (uint8_t)(i + 1), 0, LV_DIR_HOR);
        _session_tiles.push_back(tile);
        _sessions.push_back(std::make_unique<SessionPage>(tile));
    }
    _session_tile_count = count;

    _rebuild_pager(count + 1);   // +1 for the overview tile
    // Snap back to overview so a rebuild never strands the user mid-air.
    lv_tileview_set_tile_by_index(_tileview, 0, 0, LV_ANIM_OFF);
    _highlight_pager(0);
}

void WatchFace::_on_tile_change(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    lv_obj_t* active = lv_tileview_get_tile_active(self->_tileview);
    // Identify the active tile's column = page index.
    int idx = 0;
    for (size_t i = 0; i < self->_session_tiles.size(); i++) {
        if (self->_session_tiles[i] == active) { idx = (int)i + 1; break; }
    }
    self->_highlight_pager(idx);
}

void WatchFace::apply(const WatchState& state)
{
    int total   = state.sessions_total;
    int running = state.sessions_running;
    int waiting = state.sessions_waiting;

    _overview->update(total, running, waiting);

    // (Re)build session tiles to match how many sessions the daemon
    // actually described (session_details may be capped below `total`).
    int n = (int)state.session_details.size();
    if (n != _session_tile_count) {
        _rebuild_session_tiles(n);
    }

    for (int i = 0; i < n; i++) {
        const SessionInfo& s = state.session_details[i];
        ClawdState st = s.waiting ? ClawdState::Waiting
                      : s.running ? ClawdState::Working
                                  : ClawdState::Idle;
        _sessions[i]->update(
            i + 1, n, st,
            s.project,
            s.model,
            s.context_valid ? s.context_pct : -1.0f,
            s.cost_valid ? s.cost_usd : -1.0f,
            s.tool,
            s.input_tokens,
            s.output_tokens,
            s.cache_read_tokens,
            s.cache_create_tokens);
    }
}

}  // namespace clawd_watch

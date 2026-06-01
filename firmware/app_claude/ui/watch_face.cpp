/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "watch_face.h"
#include "design_tokens.h"
#include <stdio.h>

namespace clawd_watch {

namespace {

const lv_color_t kDotOff = lv_color_make(0x3a, 0x3a, 0x3a);
constexpr int kDot   = 7;    // dot diameter
constexpr int kDotGap = 9;   // center-to-center step
constexpr int kPagerY = 36;  // distance from bottom
constexpr int kScreenSize = 466;

}  // namespace

WatchFace::WatchFace()
{
    // ── Screen ───────────────────────────────────────────────────
    _screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(_screen);
    lv_obj_set_size(_screen, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_color(_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);

    // ── Container for all pages (not scrollable) ────────────────
    _container = lv_obj_create(_screen);
    lv_obj_remove_style_all(_container);
    lv_obj_set_size(_container, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_opa(_container, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_container, LV_OBJ_FLAG_CLICKABLE);

    // ── Overview page (page 0, always visible initially) ────────
    _overview = std::make_unique<OverviewPage>(_container);

    // ── Pager overlay ───────────────────────────────────────────
    _pager = lv_obj_create(_screen);
    lv_obj_remove_style_all(_pager);
    lv_obj_set_size(_pager, kScreenSize, kDot * 2);
    lv_obj_remove_flag(_pager, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(_pager, LV_ALIGN_BOTTOM_MID, 0, -kPagerY);
    lv_obj_set_flex_flow(_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_pager, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(_pager, kDotGap - kDot, 0);

    // ── Click mask (transparent, on top, captures taps) ─────────
    // Pattern from M5Stack official demo: a transparent overlay with
    // LV_OBJ_FLAG_SCROLLABLE removed reliably receives LV_EVENT_CLICKED.
    _click_mask = lv_obj_create(_screen);
    lv_obj_remove_style_all(_click_mask);
    lv_obj_set_size(_click_mask, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_opa(_click_mask, LV_OPA_1, 0);
    lv_obj_remove_flag(_click_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_click_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_click_mask, _on_tap, LV_EVENT_CLICKED, this);
    lv_obj_move_foreground(_click_mask);
    // Keep pager visible above the click mask.
    lv_obj_move_foreground(_pager);

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
        lv_obj_set_width(_dots[i], cur ? 16 : kDot);
        lv_obj_set_style_radius(_dots[i], cur ? 3 : LV_RADIUS_CIRCLE, 0);
    }
}

void WatchFace::_rebuild_session_pages(int count)
{
    // Drop old session pages.
    _sessions.clear();
    for (lv_obj_t* p : _session_pages) lv_obj_del(p);
    _session_pages.clear();

    for (int i = 0; i < count; i++) {
        // Create directly on _screen (not _container) to avoid rendering issues.
        lv_obj_t* page = lv_obj_create(_screen);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(page, kScreenSize, kScreenSize);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_pos(page, 0, kScreenSize + 100);
        _session_pages.push_back(page);
        _sessions.push_back(std::make_unique<SessionPage>(page));
    }
    _session_page_count = count;

    // Session pages were appended to _screen, pushing click_mask and pager
    // behind them.  Restore z-order so taps still reach the click mask.
    lv_obj_move_foreground(_click_mask);
    lv_obj_move_foreground(_pager);

    _rebuild_pager(count + 1);   // +1 for the overview page
    // Preserve current page if still valid; otherwise fall back to overview.
    int total = count + 1;
    int stay = (_current_page_idx < total) ? _current_page_idx : 0;
    _show_page(stay);
}

void WatchFace::_on_tap(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    self->next_page();
}

void WatchFace::next_page()
{
    int total = 1 + (int)_session_pages.size();  // overview + sessions
    int next = (_current_page_idx + 1) % total;
    _show_page(next);
}

void WatchFace::_show_page(int idx)
{
    _current_page_idx = idx;

    // Overview is page 0; sessions are pages 1..N.
    // Move inactive pages off-screen, active one to (0,0).
    if (_overview) {
        if (idx == 0) {
            lv_obj_set_pos(_overview->lvobj(), 0, 0);
        } else {
            lv_obj_set_pos(_overview->lvobj(), 0, kScreenSize + 100);
        }
    }
    for (size_t i = 0; i < _session_pages.size(); i++) {
        if ((int)i + 1 == idx) {
            lv_obj_set_pos(_session_pages[i], 0, 0);
        } else {
            lv_obj_set_pos(_session_pages[i], 0, kScreenSize + 100);
        }
    }

    _highlight_pager(idx);
}

void WatchFace::apply(const WatchState& state, bool ble_connected)
{
    int total   = state.sessions_total;
    int running = state.sessions_running;
    int waiting = state.sessions_waiting;

    // Celebration: when running drops from >0 to 0 and nothing is waiting,
    // briefly show a happy bounce before settling into idle.
    if (_prev_running > 0 && running == 0 && waiting == 0) {
        _overview->update(total, 0, 0, ClawdState::Celebrate, ble_connected);
    } else {
        _overview->update(total, running, waiting, std::nullopt, ble_connected);
    }
    _prev_running = running;

    // Update existing session pages FIRST — before checking whether to
    // rebuild.  The BLE callback thread does clear()+push_back() on
    // session_details without a lock; if we read between those two ops
    // the size is momentarily 0.  By updating first we guarantee the
    // visible page always has fresh data, and the guard below prevents
    // a spurious teardown when n==0 but we already have pages.
    int n = (int)state.session_details.size();
    int update_count = n < (int)_sessions.size() ? n : (int)_sessions.size();
    for (int i = 0; i < update_count; i++) {
        const SessionInfo& s = state.session_details[i];
        ClawdState st = s.waiting ? ClawdState::Waiting
                      : s.running ? ClawdState::Working
                                  : ClawdState::Idle;
        _sessions[i]->update(
            i + 1, n, st,
            s.title,
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

    // (Re)build session pages only when the count genuinely changed.
    // Ignore n==0 when we already have pages — that's the BLE thread
    // race (clear before push_back), not an actual session teardown.
    if (n != _session_page_count && !(n == 0 && _session_page_count > 0)) {
        _rebuild_session_pages(n);
    }
}

}  // namespace clawd_watch

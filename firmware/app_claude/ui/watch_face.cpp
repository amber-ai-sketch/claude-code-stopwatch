/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "watch_face.h"
#include "design_tokens.h"
#include <stdio.h>
#include <esp_log.h>

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

    // ── Click mask (near-transparent, on top, captures touches) ──
    // Full-screen overlay that receives all PRESSED/RELEASED events.
    // opa=1 (not TRANSP) because LVGL's hit-test misses fully transparent
    // objects — see commit e279a9f. SCROLLABLE removed so the press isn't
    // swallowed by scroll handling.
    _click_mask = lv_obj_create(_screen);
    lv_obj_remove_style_all(_click_mask);
    lv_obj_set_size(_click_mask, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_opa(_click_mask, 1, 0);
    lv_obj_remove_flag(_click_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_click_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_click_mask, _on_touch_start, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(_click_mask, _on_touch_move,  LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(_click_mask, _on_touch_end,   LV_EVENT_RELEASED, this);
    lv_obj_move_foreground(_click_mask);
    // Keep pager visible above the click mask.
    lv_obj_move_foreground(_pager);
    _cheatsheet = _build_cheatsheet();
    lv_obj_move_foreground(_cheatsheet);

    _rebuild_pager(1);
    _highlight_pager(0);

    // ── Transcript overlay (hidden initially) ─────────────────
    _transcript_panel = lv_obj_create(_screen);
    lv_obj_remove_flag(_transcript_panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(_transcript_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(_transcript_panel, kScreenSize - 40, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(_transcript_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_transcript_panel, 200, 0);
    lv_obj_set_style_radius(_transcript_panel, 12, 0);
    lv_obj_set_style_pad_all(_transcript_panel, 14, 0);
    lv_obj_align(_transcript_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_transcript_panel, LV_OBJ_FLAG_HIDDEN);

    _transcript_label = lv_label_create(_transcript_panel);
    lv_obj_set_style_text_font(_transcript_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_transcript_label, lv_color_white(), 0);
    lv_label_set_long_mode(_transcript_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_transcript_label, kScreenSize - 68);
    lv_label_set_text(_transcript_label, "");

    ESP_LOGE("GEST", "WATCHFACE CTOR DONE build=%s %s", __DATE__, __TIME__);
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

lv_obj_t* WatchFace::_build_cheatsheet()
{
    // Full-screen solid black overlay — acts as its own page.
    lv_obj_t* cs = lv_obj_create(_screen);
    lv_obj_remove_style_all(cs);
    lv_obj_set_size(cs, kScreenSize, kScreenSize);
    lv_obj_set_style_bg_color(cs, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cs, LV_OPA_COVER, 0);
    lv_obj_add_flag(cs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cs, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(cs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cs, _on_cheatsheet_tap, LV_EVENT_CLICKED, this);

    // ── Header ────────────────────────────────────────────────────
    lv_obj_t* title = lv_label_create(cs);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, kGrey, 0);
    lv_label_set_text(title, "BUTTON REFERENCE");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -122);

    // ── Divider ───────────────────────────────────────────────────
    lv_obj_t* div = lv_obj_create(cs);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 160, 1);
    lv_obj_set_style_bg_color(div, kDim, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_CENTER, 0, -100);

    // ── Left A ────────────────────────────────────────────────────
    lv_obj_t* la_key = lv_label_create(cs);
    lv_obj_set_style_text_font(la_key, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(la_key, lv_color_white(), 0);
    lv_label_set_text(la_key, "A");
    lv_obj_align(la_key, LV_ALIGN_CENTER, -60, -64);

    lv_obj_t* la_tap = lv_label_create(cs);
    lv_obj_set_style_text_font(la_tap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(la_tap, kOrange, 0);
    lv_label_set_text(la_tap, "tap");
    lv_obj_align(la_tap, LV_ALIGN_CENTER, -60, -36);

    lv_obj_t* la_tap_v = lv_label_create(cs);
    lv_obj_set_style_text_font(la_tap_v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(la_tap_v, kGrey, 0);
    lv_label_set_text(la_tap_v, "Backspace");
    lv_obj_align(la_tap_v, LV_ALIGN_CENTER, -60, -16);

    lv_obj_t* la_hold = lv_label_create(cs);
    lv_obj_set_style_text_font(la_hold, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(la_hold, kOrange, 0);
    lv_label_set_text(la_hold, "hold");
    lv_obj_align(la_hold, LV_ALIGN_CENTER, -60, 10);

    lv_obj_t* la_hold_v = lv_label_create(cs);
    lv_obj_set_style_text_font(la_hold_v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(la_hold_v, kGrey, 0);
    lv_label_set_text(la_hold_v, "Enter");
    lv_obj_align(la_hold_v, LV_ALIGN_CENTER, -60, 30);

    // ── Vertical separator ────────────────────────────────────────
    lv_obj_t* vsep = lv_obj_create(cs);
    lv_obj_remove_style_all(vsep);
    lv_obj_set_size(vsep, 1, 110);
    lv_obj_set_style_bg_color(vsep, kDim, 0);
    lv_obj_set_style_bg_opa(vsep, LV_OPA_COVER, 0);
    lv_obj_align(vsep, LV_ALIGN_CENTER, 0, -10);

    // ── Right B ───────────────────────────────────────────────────
    lv_obj_t* rb_key = lv_label_create(cs);
    lv_obj_set_style_text_font(rb_key, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(rb_key, lv_color_white(), 0);
    lv_label_set_text(rb_key, "B");
    lv_obj_align(rb_key, LV_ALIGN_CENTER, 60, -64);

    lv_obj_t* rb_tap = lv_label_create(cs);
    lv_obj_set_style_text_font(rb_tap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(rb_tap, kOrange, 0);
    lv_label_set_text(rb_tap, "tap");
    lv_obj_align(rb_tap, LV_ALIGN_CENTER, 60, -36);

    lv_obj_t* rb_tap_v = lv_label_create(cs);
    lv_obj_set_style_text_font(rb_tap_v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rb_tap_v, kGrey, 0);
    lv_label_set_text(rb_tap_v, "Esc");
    lv_obj_align(rb_tap_v, LV_ALIGN_CENTER, 60, -16);

    lv_obj_t* rb_hold = lv_label_create(cs);
    lv_obj_set_style_text_font(rb_hold, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(rb_hold, kOrange, 0);
    lv_label_set_text(rb_hold, "hold");
    lv_obj_align(rb_hold, LV_ALIGN_CENTER, 60, 10);

    lv_obj_t* rb_hold_v = lv_label_create(cs);
    lv_obj_set_style_text_font(rb_hold_v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rb_hold_v, kGrey, 0);
    lv_label_set_text(rb_hold_v, "WeChat / Mic");
    lv_obj_align(rb_hold_v, LV_ALIGN_CENTER, 60, 30);

    // ── Divider ───────────────────────────────────────────────────
    lv_obj_t* div2 = lv_obj_create(cs);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, 160, 1);
    lv_obj_set_style_bg_color(div2, kDim, 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);
    lv_obj_align(div2, LV_ALIGN_CENTER, 0, 78);

    // ── Dismiss hint ──────────────────────────────────────────────
    lv_obj_t* hint = lv_label_create(cs);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, kDimIdle, 0);
    lv_label_set_text(hint, "tap anywhere to dismiss");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 100);

    return cs;
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
        lv_obj_remove_flag(page, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(page, kScreenSize, kScreenSize);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_pos(page, 0, kScreenSize + 100);
        _session_pages.push_back(page);
        _sessions.push_back(std::make_unique<SessionPage>(page));
    }
    _session_page_count = count;

    // Session pages were appended to _screen, pushing click_mask, pager,
    // and cheatsheet behind them.  Restore z-order.
    lv_obj_move_foreground(_click_mask);
    lv_obj_move_foreground(_pager);
    if (_cheatsheet) lv_obj_move_foreground(_cheatsheet);

    _rebuild_pager(count + 1);   // +1 for the overview page
    // Preserve current page if still valid; otherwise fall back to overview.
    int total = count + 1;
    int stay = (_current_page_idx < total) ? _current_page_idx : 0;
    _show_page(stay);
}

void WatchFace::_on_cheatsheet_tap(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    if (self->_cheatsheet) lv_obj_add_flag(self->_cheatsheet, LV_OBJ_FLAG_HIDDEN);
}

void WatchFace::_on_touch_start(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    lv_indev_get_point(lv_indev_get_act(), &self->_touch_start);
    self->_touch_last = self->_touch_start;  // seed so a tap (no move) reads zero
    self->_touch_active = true;
    ESP_LOGE("GEST", "DOWN x=%d y=%d", (int)self->_touch_start.x, (int)self->_touch_start.y);
}

void WatchFace::_on_touch_move(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    // Track the live finger position throughout the drag. At RELEASED the
    // indev point is already stale (finger lifted), so we use this instead.
    lv_indev_get_point(lv_indev_get_act(), &self->_touch_last);
    ESP_LOGE("GEST", "MOVE x=%d y=%d", (int)self->_touch_last.x, (int)self->_touch_last.y);
}

void WatchFace::_on_touch_end(lv_event_t* e)
{
    auto* self = static_cast<WatchFace*>(lv_event_get_user_data(e));
    if (!self->_touch_active) return;
    self->_touch_active = false;

    int32_t dx = self->_touch_last.x - self->_touch_start.x;
    int32_t dy = self->_touch_last.y - self->_touch_start.y;
    int32_t dist = LV_MAX(LV_ABS(dx), LV_ABS(dy));
    ESP_LOGE("GEST", "UP start=(%d,%d) last=(%d,%d) dx=%d dy=%d dist=%d",
             (int)self->_touch_start.x, (int)self->_touch_start.y,
             (int)self->_touch_last.x, (int)self->_touch_last.y,
             (int)dx, (int)dy, (int)dist);

    // Tap (no meaningful movement) → next page. From the overview this
    // advances to the first session, satisfying "tap on home → session".
    if (dist < kTapMaxDist) {
        self->next_page();
        return;
    }

    // Swipe on the overview: only a downward swipe opens the cheatsheet.
    // Up / left / right are ignored. Session pages ignore swipes entirely.
    if (self->_current_page_idx == 0) {
        bool is_down = (dy > 0) && (LV_ABS(dy) > LV_ABS(dx));
        if (is_down) lv_obj_clear_flag(self->_cheatsheet, LV_OBJ_FLAG_HIDDEN);
    }
}

void WatchFace::show_cheatsheet()
{
    if (_cheatsheet) lv_obj_clear_flag(_cheatsheet, LV_OBJ_FLAG_HIDDEN);
}

void WatchFace::hide_cheatsheet()
{
    if (_cheatsheet) lv_obj_add_flag(_cheatsheet, LV_OBJ_FLAG_HIDDEN);
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
        _overview->update(total, 0, 0, ClawdState::Celebrate, ble_connected,
                          state.battery_pct, state.battery_charging);
    } else {
        _overview->update(total, running, waiting, std::nullopt, ble_connected,
                          state.battery_pct, state.battery_charging);
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

    // ── Transcript overlay ──────────────────────────────────────
    if (!state.transcript.empty() && state.transcript_at_ms != _transcript_until_ms) {
        // New transcript arrived — show it.
        lv_label_set_text(_transcript_label, state.transcript.c_str());
        lv_obj_clear_flag(_transcript_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_transcript_panel);
        lv_obj_move_foreground(_click_mask);
        lv_obj_move_foreground(_pager);
        _transcript_until_ms = state.transcript_at_ms + kTranscriptShowMs;
    }
    if (_transcript_until_ms && state.last_updated_ms > _transcript_until_ms) {
        lv_obj_add_flag(_transcript_panel, LV_OBJ_FLAG_HIDDEN);
        _transcript_until_ms = 0;
    }
}

}  // namespace clawd_watch

/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * WatchFace — the tap-cycling watch face. A plain container holds:
 *   page 0      : OverviewPage (Clawd + status + session tally), default
 *   page 1..N   : one SessionPage per active Claude Code session
 * A row of page dots at the bottom marks the current page (orange bar).
 *
 * Swipe is disabled — a transparent click-mask on top cycles on tap:
 *   overview → session 1 → … → session N → overview
 *
 * Rebuilds the session pages whenever the session count changes; metrics
 * within existing pages update in place.
 */
#pragma once
#include <lvgl.h>
#include <memory>
#include <vector>
#include "../ble/watch_state.h"
#include "overview_page.h"
#include "session_page.h"

namespace clawd_watch {

class WatchFace {
public:
    WatchFace();
    ~WatchFace();

    void show();
    void apply(const WatchState& state, bool ble_connected);
    void next_page();
    void show_cheatsheet();
    void hide_cheatsheet();
    int  current_page() const { return _current_page_idx; }

    lv_obj_t* screen() { return _screen; }

private:
    void _rebuild_session_pages(int count);
    void _rebuild_pager(int page_count);
    void _highlight_pager(int active);
    void _show_page(int idx);
    lv_obj_t* _build_cheatsheet();
    static void _on_touch_start(lv_event_t* e);
    static void _on_touch_move(lv_event_t* e);
    static void _on_touch_end(lv_event_t* e);
    static void _on_cheatsheet_tap(lv_event_t* e);

    // Touch tracking for tap-vs-swipe discrimination on the click mask.
    // _touch_last is updated every PRESSING tick because the RELEASED-time
    // indev point is stale (finger already lifted).
    lv_point_t _touch_start = {};
    lv_point_t _touch_last  = {};
    bool       _touch_active = false;
    static constexpr int32_t kTapMaxDist = 15;  // px; below = tap, above = swipe

    lv_obj_t* _cheatsheet = nullptr;

    lv_obj_t* _screen     = nullptr;
    lv_obj_t* _container  = nullptr;    // plain container, holds all pages
    lv_obj_t* _click_mask = nullptr;    // transparent overlay, captures taps
    lv_obj_t* _pager      = nullptr;    // bottom dot row, overlay
    std::vector<lv_obj_t*> _dots;

    std::unique_ptr<OverviewPage> _overview;
    std::vector<std::unique_ptr<SessionPage>> _sessions;
    std::vector<lv_obj_t*> _session_pages;  // parent obj per session page

    int _session_page_count = -1;           // -1 = not built yet
    int _current_page_idx = 0;              // 0 = overview, 1..N = sessions
    int _prev_running = 0;                  // for celebration detection

    // Voice transcript overlay — shows dictated text briefly after recording.
    lv_obj_t* _transcript_panel = nullptr;
    lv_obj_t* _transcript_label = nullptr;
    uint32_t  _transcript_until_ms = 0;     // auto-hide after this time
    static constexpr uint32_t kTranscriptShowMs = 5000;
};

}  // namespace clawd_watch

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
    void apply(const WatchState& state, bool ble_connected);  // call ~5x/s from the app loop
    void next_page();                     // cycle: overview → sessions → overview

    lv_obj_t* screen() { return _screen; }

private:
    void _rebuild_session_pages(int count);
    void _rebuild_pager(int page_count);
    void _highlight_pager(int active);
    void _show_page(int idx);
    static void _on_tap(lv_event_t* e);

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
};

}  // namespace clawd_watch

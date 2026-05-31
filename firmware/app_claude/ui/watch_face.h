/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * WatchFace — the swipeable watch face. A horizontal lv_tileview holds:
 *   tile 0      : OverviewPage (Clawd + status + session tally), default
 *   tile 1..N   : one SessionPage per active Claude Code session
 * A row of page dots at the bottom marks the current tile (orange bar).
 *
 * Rebuilds the session tiles whenever the session count changes; metrics
 * within existing tiles update in place. The user lands on the overview
 * tile and swipes right to inspect individual sessions.
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
    void apply(const WatchState& state);  // call ~5x/s from the app loop

    lv_obj_t* screen() { return _screen; }

private:
    void _rebuild_session_tiles(int count);
    void _rebuild_pager(int page_count);
    void _highlight_pager(int active);
    static void _on_tile_change(lv_event_t* e);

    lv_obj_t* _screen   = nullptr;
    lv_obj_t* _tileview = nullptr;
    lv_obj_t* _pager    = nullptr;          // bottom dot row, overlay
    std::vector<lv_obj_t*> _dots;

    std::unique_ptr<OverviewPage> _overview;
    std::vector<std::unique_ptr<SessionPage>> _sessions;
    std::vector<lv_obj_t*> _session_tiles;  // parent tile per session page

    int _session_tile_count = -1;           // -1 = not built yet
};

}  // namespace clawd_watch

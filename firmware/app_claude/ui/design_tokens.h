/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 *
 * Shared design tokens — colors, spacing, and visual constants used across
 * all UI components. Single source of truth; include this instead of
 * defining colors locally.
 *
 * Emotional palette: warm Claude orange on black, never red/alarm.
 * "Peace + proactive" tone throughout.
 */
#pragma once
#include <lvgl.h>

namespace clawd_watch {

// ─── Core palette ─────────────────────────────────────────────
inline const lv_color_t kOrange    = lv_color_make(0xD9, 0x77, 0x57);
inline const lv_color_t kOrangeDim = lv_color_make(0x9c, 0x6b, 0x56);
inline const lv_color_t kAmber     = lv_color_make(0xFF, 0xBB, 0x55);  // waiting chip — brighter than kOrange
inline const lv_color_t kGrey      = lv_color_make(0x8a, 0x8a, 0x8a);
inline const lv_color_t kDim       = lv_color_make(0x7a, 0x7a, 0x7a);
inline const lv_color_t kDimIdle   = lv_color_make(0x55, 0x55, 0x55);  // idle chip text — recedes

// ─── Chip backgrounds ─────────────────────────────────────────
inline const lv_color_t kChipBgWorking = lv_color_make(0x33, 0x22, 0x1a);  // dark brown
inline const lv_color_t kChipBgWaiting = lv_color_make(0x44, 0x33, 0x11);  // warmer, brighter
inline const lv_color_t kChipBgIdle    = lv_color_make(0x1a, 0x1a, 0x1a);  // near-black, recedes

// ─── Arc ring ─────────────────────────────────────────────────
inline const lv_color_t kArcTrack     = lv_color_make(0x1f, 0x18, 0x14);  // dark brown track
inline const lv_color_t kArcUnknown   = lv_color_make(0x1a, 0x14, 0x10);  // dimmer — "no data"

// ─── Background ───────────────────────────────────────────────
inline const lv_color_t kBgBlack = lv_color_black();

}  // namespace clawd_watch

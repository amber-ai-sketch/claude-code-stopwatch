/*
 * SPDX-FileCopyrightText: 2026 ym (claude-code-stopwatch)
 * SPDX-License-Identifier: MIT
 */
#include "protocol_parse.h"

namespace clawd_watch {

namespace {

inline std::string sv(JsonVariantConst v, const std::string& fallback = "")
{
    const char* p = v.is<const char*>() ? v.as<const char*>() : nullptr;
    return p ? std::string(p) : fallback;
}

}  // namespace

bool apply_json_line(const char* line, WatchState& state, uint32_t now_ms)
{
    if (!line || line[0] != '{') return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) return false;

    // ─── time sync ──────────────────────────────────────────────
    JsonArrayConst t = doc["time"];
    if (!t.isNull() && t.size() == 2) {
        // M4 doesn't drive RTC. The watch demo's RTC is set elsewhere.
        // Just note that we got it as a connection signal.
        state.last_updated_ms = now_ms;
        state.connected       = true;
        return false;
    }

    // ─── commands ───────────────────────────────────────────────
    const char* cmd = doc["cmd"];
    if (cmd) {
        // transcript echo: daemon sends the dictated text after voice input.
        if (strcmp(cmd, "transcript") == 0) {
            const char* text = doc["text"];
            if (text) {
                state.transcript    = text;
                state.transcript_at_ms = now_ms;
            }
        }
        // owner / name / unpair etc — handled at a higher layer (it
        // needs to send acks via NUS). We only mark the link alive.
        state.last_updated_ms = now_ms;
        state.connected       = true;
        return false;
    }

    // ─── heartbeat snapshot ────────────────────────────────────
    state.sessions_total   = doc["total"]   | state.sessions_total;
    state.sessions_running = doc["running"] | state.sessions_running;
    state.sessions_waiting = doc["waiting"] | state.sessions_waiting;

    JsonVariantConst msg_v = doc["msg"];
    if (msg_v.is<const char*>()) state.msg = msg_v.as<const char*>();

    // Statusline-derived fields (compact key names, see protocol.py)
    JsonVariantConst cost_v = doc["cost"];
    if (cost_v.is<float>() || cost_v.is<int>()) {
        state.cost_usd   = cost_v.as<float>();
        state.cost_valid = true;
    }
    JsonVariantConst ctx_v = doc["ctx"];
    if (ctx_v.is<float>() || ctx_v.is<int>()) {
        state.context_pct   = ctx_v.as<float>();
        state.context_valid = true;
    }
    JsonVariantConst r5h = doc["r5h"];
    if (r5h.is<float>() || r5h.is<int>()) {
        state.rate_5h_pct   = r5h.as<float>();
        state.rate_5h_valid = true;
    }
    JsonVariantConst r7d = doc["r7d"];
    if (r7d.is<float>() || r7d.is<int>()) {
        state.rate_7d_pct   = r7d.as<float>();
        state.rate_7d_valid = true;
    }
    JsonVariantConst model_v = doc["model"];
    if (model_v.is<const char*>()) state.model_name = model_v.as<const char*>();

    JsonVariantConst tool_v = doc["tool"];
    if (tool_v.is<const char*>()) state.current_tool = tool_v.as<const char*>();
    else if (doc.containsKey("tool")) state.current_tool.clear();

    // ─── per-session detail array ──────────────────────────────
    JsonArrayConst sessions = doc["sessions"];
    if (!sessions.isNull()) {
        state.session_details.clear();
        for (JsonObjectConst so : sessions) {
            SessionInfo info;
            info.sid     = sv(so["sid"]);
            info.running = (so["run"] | 0) != 0;
            info.waiting = (so["wait"] | 0) != 0;
            JsonVariantConst c = so["ctx"];
            if (c.is<float>() || c.is<int>()) { info.context_pct = c.as<float>(); info.context_valid = true; }
            JsonVariantConst m = so["cost"];
            if (m.is<float>() || m.is<int>()) { info.cost_usd = m.as<float>(); info.cost_valid = true; }
            info.model   = sv(so["model"]);
            info.tool    = sv(so["tool"]);
            info.title   = sv(so["title"]);
            info.project = sv(so["proj"]);
            if (so["tin"].is<int>())  info.input_tokens        = so["tin"].as<int>();
            if (so["tout"].is<int>()) info.output_tokens       = so["tout"].as<int>();
            if (so["cr"].is<int>())   info.cache_read_tokens   = so["cr"].as<int>();
            if (so["cw"].is<int>())   info.cache_create_tokens = so["cw"].as<int>();
            state.session_details.push_back(info);
        }
    }

    // ─── prompt (pending approval) ─────────────────────────────
    JsonObjectConst pr = doc["prompt"];
    if (!pr.isNull()) {
        state.prompt_id   = sv(pr["id"]);
        state.prompt_tool = sv(pr["tool"]);
        state.prompt_hint = sv(pr["hint"]);
    } else if (doc.containsKey("prompt")) {
        state.prompt_id.clear();
        state.prompt_tool.clear();
        state.prompt_hint.clear();
    }

    state.last_updated_ms = now_ms;
    state.connected       = true;
    return true;
}

}  // namespace clawd_watch

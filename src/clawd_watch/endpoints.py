"""HTTP route handlers for the clawd-watch daemon.

Extracted from daemon.py to keep the Daemon class focused on BLE, audio,
and heartbeat orchestration.  Each handler is a plain async function that
receives the aiohttp request and the Daemon instance.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from dataclasses import asdict
from typing import Any, TYPE_CHECKING

from aiohttp import web

from . import DAEMON_APPROVAL_TIMEOUT
from .audio_sink import DEFAULT_TEST_PHRASE, AudioSinkError, play_spoken_text_to_blackhole
from .config import clear_address, load_address, save_address
from .protocol import short_hint_for_tool

if TYPE_CHECKING:
    from .daemon import Daemon

log = logging.getLogger("clawd_watch.endpoints")


# ─── helpers ─────────────────────────────────────────────────────

async def _read_payload(req: web.Request) -> dict[str, Any]:
    raw = await req.read()
    if not raw:
        return {}
    try:
        return json.loads(raw)
    except Exception as e:
        log.warning("bad JSON payload on %s (%d bytes): %s", req.path, len(raw), e)
        raise web.HTTPBadRequest(text="invalid JSON body")


# ─── hook event handlers ─────────────────────────────────────────

async def on_session_start(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    sid = p.get("session_id") or "unknown"
    d.state.session_seen(sid)
    log.info("SessionStart sid=%s source=%s", sid, p.get("source"))
    return web.json_response({"ok": True})


async def on_session_end(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    sid = p.get("session_id") or "unknown"
    d.state.session_end(sid)
    log.info("SessionEnd sid=%s", sid)
    return web.json_response({"ok": True})


async def on_user_prompt(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    sid = p.get("session_id") or "unknown"
    d.state.session_set_running(sid, True)
    log.info("UserPromptSubmit sid=%s", sid)
    return web.json_response({"ok": True})


async def on_stop(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    sid = p.get("session_id") or "unknown"
    d.state.session_set_running(sid, False)
    d.state.set_current_tool(None, sid)
    log.info("Stop sid=%s reason=%s", sid, p.get("stop_reason"))
    return web.json_response({"ok": True})


async def on_pre_tool(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    sid = p.get("session_id") or "unknown"
    tool = p.get("tool_name") or "?"
    tool_input = p.get("tool_input") or {}
    approval_id = p.get("tool_use_id") or f"{sid}-{int(time.time()*1000)}"
    hint = short_hint_for_tool(tool, tool_input)

    d.state.set_current_tool(tool, sid)

    pending = d.state.register_pending(approval_id, sid, tool, hint)
    log.info("PreToolUse sid=%s tool=%s id=%s hint=%r", sid, tool, approval_id, hint)

    try:
        decision = await asyncio.wait_for(pending.future, timeout=DAEMON_APPROVAL_TIMEOUT)
    except asyncio.TimeoutError:
        d.state.pending.pop(approval_id, None)
        d.state.changed.set()
        log.info("PreToolUse sid=%s tool=%s -> ask (timeout)", sid, tool)
        return web.json_response(
            {"decision": "ask", "reason": "watch timeout, defer to CLI"}
        )

    log.info("PreToolUse sid=%s tool=%s -> %s", sid, tool, decision)
    return web.json_response(
        {"decision": decision, "reason": f"watch button ({decision})"}
    )


async def on_post_tool(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    approval_id = p.get("tool_use_id") or ""
    if approval_id and approval_id in d.state.pending:
        d.state.pending.pop(approval_id, None)
        d.state.changed.set()
    return web.json_response({"ok": True})


async def on_statusline(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    snapshot = {
        k: p.get(k)
        for k in ("session_id", "cost_usd", "context_pct", "rate_5h_pct",
                  "rate_7d_pct", "model_name", "project",
                  "session_name", "worktree_name", "agent_name",
                  "input_tokens", "output_tokens",
                  "cache_read_tokens", "cache_create_tokens")
        if p.get(k) is not None
    }
    d.state.update_statusline(snapshot)
    log.debug("statusline %s", snapshot)
    return web.json_response({"ok": True})


async def on_get_status(_req: web.Request, d: Daemon) -> web.Response:
    return web.json_response(
        {
            "connected": d.ble.connected,
            "device_name": d.ble.device_name,
            "address": d.ble.address,
            "uptime_s": int(time.time() - d.state.started),
            "sessions": [
                {
                    "session_id": s.session_id,
                    "running": s.running,
                    "last_seen": s.last_seen,
                }
                for s in d.state.sessions.values()
            ],
            "pending": [
                {
                    "id": p.approval_id,
                    "session_id": p.session_id,
                    "tool": p.tool,
                    "hint": p.hint,
                    "created": p.created,
                }
                for p in d.state.pending.values()
            ],
            "approvals_total": d.state.approvals_total,
            "denials_total": d.state.denials_total,
            "statusline": d.state.statusline,
            "daily": d.state.daily_totals(),
            "current_tool": d.state.current_tool,
            "voice": d.voice_status,
            "mode": d.mode,
        }
    )


async def on_test_prompt(_req: web.Request, d: Daemon) -> web.Response:
    approval_id = f"test-{int(time.time()*1000)}"
    pending = d.state.register_pending(
        approval_id, "test-session", "Bash", "echo 'hello from clawd-watch test'"
    )
    try:
        decision = await asyncio.wait_for(pending.future, timeout=DAEMON_APPROVAL_TIMEOUT)
    except asyncio.TimeoutError:
        d.state.pending.pop(approval_id, None)
        d.state.changed.set()
        decision = "timeout"
    return web.json_response({"decision": decision, "id": approval_id})


async def on_voice_test(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    text = p.get("text") or DEFAULT_TEST_PHRASE
    device_index = p.get("device_index")
    if device_index is not None and not isinstance(device_index, int):
        raise web.HTTPBadRequest(reason="device_index must be an integer")

    d.voice_status = {
        "state": "playing_test_audio",
        "text": text,
        "device_index": device_index,
        "started": time.time(),
    }
    log.info("voice-test start device=%s text=%r", device_index, text)
    try:
        result = await play_spoken_text_to_blackhole(
            text, device_index=device_index
        )
    except AudioSinkError as e:
        d.voice_status = {
            "state": "error",
            "error": str(e),
            "updated": time.time(),
        }
        raise web.HTTPInternalServerError(reason=str(e))

    d.voice_status = {
        "state": "idle" if result.ok else "error",
        "last_test": asdict(result),
        "updated": time.time(),
    }
    log.info(
        "voice-test done ok=%s device=%s rc=%s",
        result.ok,
        result.device_index,
        result.returncode,
    )
    return web.json_response(asdict(result))


# ─── BLE control (driven by the menu-bar UI / CLI) ─────────────

async def on_set_mode(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    mode = p.get("mode")
    if mode not in ("trigger", "mic"):
        raise web.HTTPBadRequest(reason=f"mode must be 'trigger' or 'mic', got {mode!r}")
    d.mode = mode
    log.info("button mode set to %r", mode)
    return web.json_response({"ok": True, "mode": d.mode})


async def on_scan(_req: web.Request, d: Daemon) -> web.Response:
    devices = await d.ble.scan(timeout=6.0)
    log.info("scan found %d device(s)", len(devices))
    return web.json_response({"devices": devices})


async def on_reconnect(_req: web.Request, d: Daemon) -> web.Response:
    d.ble.request_reconnect()
    return web.json_response({"ok": True, "address": d.ble.address})


async def on_forget(_req: web.Request, d: Daemon) -> web.Response:
    clear_address()
    d.ble.forget()
    log.info("forgot watch address; back to name-prefix scan")
    return web.json_response({"ok": True})


async def on_connect(req: web.Request, d: Daemon) -> web.Response:
    p = await _read_payload(req)
    address = p.get("address")
    if not address or not isinstance(address, str):
        raise web.HTTPBadRequest(reason="address (string) required")
    save_address(address)
    d.ble.use_address(address)
    log.info("manual connect to %s", address)
    return web.json_response({"ok": True, "address": address})


# ─── route registration ─────────────────────────────────────────

def register_routes(app: web.Application, d: Daemon) -> None:
    """Register all HTTP routes on *app*, binding each handler to *d*."""
    def _wrap(handler):
        async def wrapper(req: web.Request) -> web.Response:
            return await handler(req, d)
        return wrapper

    app.router.add_post("/SessionStart", _wrap(on_session_start))
    app.router.add_post("/SessionEnd", _wrap(on_session_end))
    app.router.add_post("/UserPromptSubmit", _wrap(on_user_prompt))
    app.router.add_post("/Stop", _wrap(on_stop))
    app.router.add_post("/PreToolUse", _wrap(on_pre_tool))
    app.router.add_post("/PostToolUse", _wrap(on_post_tool))
    app.router.add_post("/statusline", _wrap(on_statusline))
    app.router.add_get("/status", _wrap(on_get_status))
    app.router.add_post("/test-prompt", _wrap(on_test_prompt))
    app.router.add_post("/voice-test", _wrap(on_voice_test))
    app.router.add_post("/mode", _wrap(on_set_mode))
    app.router.add_get("/scan", _wrap(on_scan))
    app.router.add_post("/reconnect", _wrap(on_reconnect))
    app.router.add_post("/forget", _wrap(on_forget))
    app.router.add_post("/connect", _wrap(on_connect))

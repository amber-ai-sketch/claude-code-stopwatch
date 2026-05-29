"""clawd-watch daemon: BLE link + HTTP endpoints for hooks + statusline.

Architecture:

  Claude Code            clawd-watch-daemon              M5Stopwatch
  ─────────────          ──────────────────              ───────────
   hooks (settings.json)  launchd, persistent             BLE peripheral
       │                       │                              │
       │ SessionStart           │                              │
       │ UserPromptSubmit       │                              │
       │ PreToolUse             │                              │
       │ Stop / SessionEnd      │                              │
       │ statusLine.command     │                              │
       ▼                        │                              │
   clawd-watch-hook ── HTTP ▶ 127.0.0.1:9877                   │
   clawd-statusline ── HTTP ▶                                  │
                              ▼                                │
                          State (sessions + statusline +        │
                          current_tool + pending approvals)    │
                              │                                │
                              │ heartbeat() ─ BLE ─▶  device   │
                              ▲ ───── BLE notify ─────         │
                                {"cmd":"permission","decision":...}

The device replies to PreToolUse approvals with a NUS notify; the hook
turn waits up to APPROVAL_TIMEOUT seconds for the future to resolve.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import time
from typing import Any

from aiohttp import web

from . import HTTP_HOST, HTTP_PORT, LOG_PATH, DEVICE_NAME_PREFIX
from .ble import WatchBLE
from .keyinject import key_down, key_tap, key_up
from .protocol import (
    generic_ack,
    heartbeat,
    short_hint_for_tool,
    status_ack,
)
from .state import State

log = logging.getLogger("clawd_watch.daemon")


APPROVAL_TIMEOUT = 30.0   # must match the hook timeout chain (35s in settings.json)
HEARTBEAT_INTERVAL = 10.0  # keepalive when nothing has changed


class Daemon:
    def __init__(self) -> None:
        self.state = State()
        self.ble = WatchBLE(
            name_prefix=DEVICE_NAME_PREFIX,
            address=os.environ.get("CLAWD_WATCH_ADDRESS") or None,
            owner_name=os.environ.get("CLAWD_WATCH_OWNER")
                or os.environ.get("USER", ""),
            on_message=self._on_ble_message,
            on_connect=self._on_ble_connect,
        )

    # ─── BLE plumbing ──────────────────────────────────────────

    async def _on_ble_connect(self) -> None:
        await self._send_heartbeat()

    async def _on_ble_message(self, msg: dict[str, Any]) -> None:
        if msg.get("cmd") == "permission":
            approval_id = msg.get("id", "")
            raw = msg.get("decision", "ask")
            decision = {"once": "allow", "always": "allow", "deny": "deny"}.get(raw, "ask")
            resolved = self.state.resolve_pending(approval_id, decision)
            log.info("permission %s for %s (resolved=%s)", decision, approval_id, resolved)
            await self._send_heartbeat()
            return

        if msg.get("cmd") == "status":
            up = int(time.time() - self.state.started)
            await self.ble.send(
                status_ack(
                    name="clawd-watch",
                    secure=False,
                    approvals=self.state.approvals_total,
                    denials=self.state.denials_total,
                    up_seconds=up,
                )
            )
            return

        cmd = msg.get("cmd")

        # Plan B: device-driven keystroke injection. macOS won't accept
        # input from a BLE-HID-only device that wasn't paired through
        # the Bluetooth UI, so the device sends NUS commands and we
        # inject Quartz events here. Same input pipeline as the built-in
        # keyboard from the OS's perspective.
        if cmd == "key_tap":
            key_tap(msg.get("mod"), msg.get("key", ""))
            return
        if cmd == "key_down":
            key_down(msg.get("mod"), msg.get("key", ""))
            return
        if cmd == "key_up":
            key_up(msg.get("mod"), msg.get("key", ""))
            return

        if cmd in ("name", "owner", "unpair"):
            await self.ble.send(generic_ack(cmd, ok=True))
            return

        log.debug("unhandled device message: %s", msg)

    async def _send_heartbeat(self) -> None:
        snap = self.state.heartbeat_snapshot()
        await self.ble.send(
            heartbeat(
                total=snap["total"],
                running=snap["running"],
                waiting=snap["waiting"],
                msg=snap["msg"],
                prompt=snap["prompt"],
                cost_usd=snap.get("cost_usd"),
                context_pct=snap.get("context_pct"),
                rate_5h_pct=snap.get("rate_5h_pct"),
                rate_7d_pct=snap.get("rate_7d_pct"),
                model_name=snap.get("model_name"),
                current_tool=snap.get("current_tool"),
            )
        )

    async def _heartbeat_loop(self) -> None:
        while True:
            try:
                await asyncio.wait_for(
                    self.state.changed.wait(), timeout=HEARTBEAT_INTERVAL
                )
            except asyncio.TimeoutError:
                pass
            self.state.changed.clear()
            self.state.prune_stale()
            if self.ble.connected:
                await self._send_heartbeat()

    # ─── HTTP endpoints ────────────────────────────────────────

    def _build_app(self) -> web.Application:
        app = web.Application()
        app.router.add_post("/SessionStart", self._on_session_start)
        app.router.add_post("/SessionEnd", self._on_session_end)
        app.router.add_post("/UserPromptSubmit", self._on_user_prompt)
        app.router.add_post("/Stop", self._on_stop)
        app.router.add_post("/PreToolUse", self._on_pre_tool)
        app.router.add_post("/PostToolUse", self._on_post_tool)
        app.router.add_post("/statusline", self._on_statusline)
        app.router.add_get("/status", self._on_get_status)
        app.router.add_post("/test-prompt", self._on_test_prompt)
        return app

    async def _read_payload(self, req: web.Request) -> dict[str, Any]:
        try:
            raw = await req.read()
            if not raw:
                return {}
            return json.loads(raw)
        except Exception as e:
            log.warning("bad payload on %s: %s", req.path, e)
            return {}

    async def _on_session_start(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        sid = p.get("session_id") or "unknown"
        self.state.session_seen(sid)
        log.info("SessionStart sid=%s source=%s", sid, p.get("source"))
        return web.json_response({"ok": True})

    async def _on_session_end(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        sid = p.get("session_id") or "unknown"
        self.state.session_end(sid)
        log.info("SessionEnd sid=%s", sid)
        return web.json_response({"ok": True})

    async def _on_user_prompt(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        sid = p.get("session_id") or "unknown"
        self.state.session_set_running(sid, True)
        log.info("UserPromptSubmit sid=%s", sid)
        return web.json_response({"ok": True})

    async def _on_stop(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        sid = p.get("session_id") or "unknown"
        self.state.session_set_running(sid, False)
        # Turn ended → no tool currently running. Drop the device out of busy.*.
        self.state.set_current_tool(None)
        log.info("Stop sid=%s reason=%s", sid, p.get("stop_reason"))
        return web.json_response({"ok": True})

    async def _on_pre_tool(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        sid = p.get("session_id") or "unknown"
        tool = p.get("tool_name") or "?"
        tool_input = p.get("tool_input") or {}
        approval_id = p.get("tool_use_id") or f"{sid}-{int(time.time()*1000)}"
        hint = short_hint_for_tool(tool, tool_input)

        # Drive busy.* motion graphic on the device.
        self.state.set_current_tool(tool)

        pending = self.state.register_pending(approval_id, sid, tool, hint)
        log.info("PreToolUse sid=%s tool=%s id=%s hint=%r", sid, tool, approval_id, hint)

        try:
            decision = await asyncio.wait_for(pending.future, timeout=APPROVAL_TIMEOUT)
        except asyncio.TimeoutError:
            self.state.pending.pop(approval_id, None)
            self.state.changed.set()
            log.info("PreToolUse sid=%s tool=%s -> ask (timeout)", sid, tool)
            return web.json_response(
                {"decision": "ask", "reason": "watch timeout, defer to CLI"}
            )

        log.info("PreToolUse sid=%s tool=%s -> %s", sid, tool, decision)
        return web.json_response(
            {"decision": decision, "reason": f"watch button ({decision})"}
        )

    async def _on_post_tool(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        approval_id = p.get("tool_use_id") or ""
        if approval_id and approval_id in self.state.pending:
            self.state.pending.pop(approval_id, None)
            self.state.changed.set()
        return web.json_response({"ok": True})

    async def _on_statusline(self, req: web.Request) -> web.Response:
        """Receive normalized statusline payload from clawd-statusline."""
        p = await self._read_payload(req)
        snapshot = {
            k: p.get(k)
            for k in ("cost_usd", "context_pct", "rate_5h_pct",
                      "rate_7d_pct", "model_name")
            if p.get(k) is not None
        }
        self.state.update_statusline(snapshot)
        log.debug("statusline %s", snapshot)
        return web.json_response({"ok": True})

    async def _on_get_status(self, _req: web.Request) -> web.Response:
        return web.json_response(
            {
                "connected": self.ble.connected,
                "device_name": self.ble.device_name,
                "uptime_s": int(time.time() - self.state.started),
                "sessions": [
                    {
                        "session_id": s.session_id,
                        "running": s.running,
                        "last_seen": s.last_seen,
                    }
                    for s in self.state.sessions.values()
                ],
                "pending": [
                    {
                        "id": p.approval_id,
                        "session_id": p.session_id,
                        "tool": p.tool,
                        "hint": p.hint,
                        "created": p.created,
                    }
                    for p in self.state.pending.values()
                ],
                "approvals_total": self.state.approvals_total,
                "denials_total": self.state.denials_total,
                "statusline": self.state.statusline,
                "current_tool": self.state.current_tool,
            }
        )

    async def _on_test_prompt(self, _req: web.Request) -> web.Response:
        approval_id = f"test-{int(time.time()*1000)}"
        pending = self.state.register_pending(
            approval_id, "test-session", "Bash", "echo 'hello from clawd-watch test'"
        )
        try:
            decision = await asyncio.wait_for(pending.future, timeout=APPROVAL_TIMEOUT)
        except asyncio.TimeoutError:
            self.state.pending.pop(approval_id, None)
            self.state.changed.set()
            decision = "timeout"
        return web.json_response({"decision": decision, "id": approval_id})

    # ─── orchestration ─────────────────────────────────────────

    async def run(self) -> None:
        app = self._build_app()
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, HTTP_HOST, HTTP_PORT)
        await site.start()
        log.info("HTTP listening on http://%s:%d", HTTP_HOST, HTTP_PORT)

        tasks = [
            asyncio.create_task(self.ble.run_forever(), name="ble"),
            asyncio.create_task(self._heartbeat_loop(), name="heartbeat"),
        ]

        stop_event = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop_event.set)
            except NotImplementedError:
                pass

        await stop_event.wait()
        log.info("shutting down")
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await runner.cleanup()


def _setup_logging() -> None:
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
    except Exception:
        pass
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    try:
        handlers.append(logging.FileHandler(LOG_PATH))
    except Exception:
        pass
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=handlers,
    )


def main() -> None:
    _setup_logging()
    try:
        asyncio.run(Daemon().run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

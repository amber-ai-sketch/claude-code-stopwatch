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

from dataclasses import asdict

from aiohttp import web

from . import HTTP_HOST, HTTP_PORT, LOG_PATH, DEVICE_NAME_PREFIX
from .audio_debug import DEBUG_WAV_DIR, finish_and_dump
from .audio_receiver import AudioCapture, AudioFrameError, TYPE_STREAM_START
from .audio_sink import DEFAULT_TEST_PHRASE, AudioSinkError, play_spoken_text_to_blackhole
from .ble import WatchBLE
from .config import clear_address, load_address, save_address
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
        # Prefer an explicit env override, then the address we persisted on the
        # last successful connect, then None (blind scan by name).
        address = os.environ.get("CLAWD_WATCH_ADDRESS") or load_address()
        self.ble = WatchBLE(
            name_prefix=DEVICE_NAME_PREFIX,
            address=address or None,
            owner_name=os.environ.get("CLAWD_WATCH_OWNER")
                or os.environ.get("USER", ""),
            on_message=self._on_ble_message,
            on_connect=self._on_ble_connect,
            on_audio_frame=self._on_audio_frame,
        )
        # Button mode: how a raw "btn right down/up" event from the watch is
        # interpreted. "trigger" → inject WeChat dictation Shift+Space (the
        # watch is a remote; the Mac mic records). "mic" → on-device mic
        # recording (phase 2, not wired yet). Default trigger: usable today.
        self.mode = "trigger"
        self.voice_status: dict[str, Any] = {"state": "idle"}
        # Phase-1 audio debug: one capture per push-to-talk stream. Reset on
        # STREAM_START, dumped to wav + stats on STREAM_END.
        self._audio_capture: AudioCapture | None = None
        self._loop: asyncio.AbstractEventLoop | None = None

    # ─── BLE plumbing ──────────────────────────────────────────

    async def _on_ble_connect(self) -> None:
        # Remember whatever address the link actually resolved to so the next
        # boot connects directly. A disk-write failure must not break a working
        # link, so log it loudly rather than letting it propagate.
        if self.ble.address:
            try:
                save_address(self.ble.address)
            except Exception as e:
                log.warning("could not persist watch address %s: %s",
                            self.ble.address, e)
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

        # Raw button event from the watch (dumb terminal). The daemon, not the
        # device, decides what a right long-press means per current mode.
        if cmd == "btn":
            self._handle_button(msg.get("key", ""), msg.get("edge", ""))
            return

        if cmd in ("name", "owner", "unpair"):
            await self.ble.send(generic_ack(cmd, ok=True))
            return

        log.debug("unhandled device message: %s", msg)

    def _handle_button(self, key: str, edge: str) -> None:
        """Interpret a raw watch button event per the current mode.

        Only the right long-press is wired. In "trigger" mode it holds
        Shift+Space for the duration of the press so WeChat dictation records
        while held and stops on release.
        """
        if key != "right" or edge not in ("down", "up"):
            log.warning("unhandled btn event: key=%r edge=%r", key, edge)
            return

        if self.mode == "trigger":
            if edge == "down":
                key_down("shift", "space")
            else:
                key_up("shift", "space")
            return

        if self.mode == "mic":
            # Phase 2: drive on-device mic recording here. Not wired yet —
            # surface it loudly rather than silently doing nothing.
            log.warning("mic mode not implemented; ignoring btn %s", edge)
            return

        raise ValueError(f"unknown mode: {self.mode!r}")

    def _on_audio_frame(self, raw: bytes) -> None:
        # Runs in bleak's sync notify context — keep it fast. A new STREAM_START
        # opens a fresh capture; STREAM_END schedules the wav dump on the loop
        # (wave I/O must not block the notify callback).
        try:
            if raw and raw[0] == TYPE_STREAM_START:
                self._audio_capture = AudioCapture()
            if self._audio_capture is None:
                # Frame arrived without a start (mid-stream connect); ignore
                # until the next stream begins rather than guess.
                return
            self._audio_capture.feed(raw)
            if self._audio_capture.ended:
                capture, self._audio_capture = self._audio_capture, None
                if self._loop is not None:
                    self._loop.call_soon_threadsafe(self._dump_audio, capture)
        except AudioFrameError as e:
            # A malformed frame means link/encoder breakage. Drop the partial
            # stream loudly rather than writing corrupt audio.
            log.error("audio frame error, dropping stream: %s", e)
            self._audio_capture = None

    def _dump_audio(self, capture: AudioCapture) -> None:
        ts = time.strftime("%Y%m%d-%H%M%S")
        wav_path = DEBUG_WAV_DIR / f"ptt-{ts}.wav"
        try:
            finish_and_dump(capture, wav_path)
        except Exception as e:
            log.error("audio dump failed: %s", e)

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
                sessions=snap.get("sessions"),
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
        app.router.add_post("/voice-test", self._on_voice_test)
        app.router.add_post("/mode", self._on_set_mode)
        app.router.add_get("/scan", self._on_scan)
        app.router.add_post("/reconnect", self._on_reconnect)
        app.router.add_post("/forget", self._on_forget)
        app.router.add_post("/connect", self._on_connect)
        return app

    async def _read_payload(self, req: web.Request) -> dict[str, Any]:
        raw = await req.read()
        if not raw:
            return {}
        try:
            return json.loads(raw)
        except Exception as e:
            log.warning("bad JSON payload on %s (%d bytes): %s", req.path, len(raw), e)
            raise web.HTTPBadRequest(text=f"invalid JSON: {e}")

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
        self.state.set_current_tool(None, sid)
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
        self.state.set_current_tool(tool, sid)

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
            for k in ("session_id", "cost_usd", "context_pct", "rate_5h_pct",
                      "rate_7d_pct", "model_name", "project",
                      "input_tokens", "output_tokens",
                      "cache_read_tokens", "cache_create_tokens")
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
                "address": self.ble.address,
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
                "voice": self.voice_status,
                "mode": self.mode,
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

    async def _on_voice_test(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        text = p.get("text") or DEFAULT_TEST_PHRASE
        device_index = p.get("device_index")
        if device_index is not None and not isinstance(device_index, int):
            raise web.HTTPBadRequest(reason="device_index must be an integer")

        self.voice_status = {
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
            self.voice_status = {
                "state": "error",
                "error": str(e),
                "updated": time.time(),
            }
            raise web.HTTPInternalServerError(reason=str(e))

        self.voice_status = {
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

    # ─── BLE control (driven by the menu-bar UI / CLI) ─────────

    async def _on_set_mode(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        mode = p.get("mode")
        if mode not in ("trigger", "mic"):
            raise web.HTTPBadRequest(reason=f"mode must be 'trigger' or 'mic', got {mode!r}")
        self.mode = mode
        log.info("button mode set to %r", mode)
        return web.json_response({"ok": True, "mode": self.mode})

    async def _on_scan(self, _req: web.Request) -> web.Response:
        devices = await self.ble.scan(timeout=6.0)
        log.info("scan found %d device(s)", len(devices))
        return web.json_response({"devices": devices})

    async def _on_reconnect(self, _req: web.Request) -> web.Response:
        self.ble.request_reconnect()
        return web.json_response({"ok": True, "address": self.ble.address})

    async def _on_forget(self, _req: web.Request) -> web.Response:
        clear_address()
        self.ble.forget()
        log.info("forgot watch address; back to name-prefix scan")
        return web.json_response({"ok": True})

    async def _on_connect(self, req: web.Request) -> web.Response:
        p = await self._read_payload(req)
        address = p.get("address")
        if not address or not isinstance(address, str):
            raise web.HTTPBadRequest(reason="address (string) required")
        save_address(address)  # persist before the attempt so the choice sticks
        self.ble.use_address(address)
        log.info("manual connect to %s", address)
        return web.json_response({"ok": True, "address": address})

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
        # Audio notify callback (bleak sync context) schedules wav dumps here.
        self._loop = loop
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
    except Exception as e:
        print(f"clawd-watch: cannot create log dir: {e}", file=sys.stderr)
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    try:
        handlers.append(logging.FileHandler(LOG_PATH))
    except Exception as e:
        print(f"clawd-watch: cannot open log file {LOG_PATH}: {e}", file=sys.stderr)
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

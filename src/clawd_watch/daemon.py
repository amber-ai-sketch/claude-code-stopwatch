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
import logging
import os
import signal
import time
from typing import Any

from aiohttp import web

from . import (
    HTTP_HOST, HTTP_PORT, LOG_PATH, DEVICE_NAME_PREFIX,
    HEARTBEAT_INTERVAL,
)
from .audio_debug import DEBUG_WAV_DIR, finish_and_dump
from .audio_receiver import AudioCapture, AudioFrameError, TYPE_STREAM_START
from .ble import WatchBLE
from .config import load_address, save_address
from .endpoints import register_routes
from .keyinject import key_down, key_up
from .protocol import generic_ack, heartbeat, status_ack
from .state import State

log = logging.getLogger("clawd_watch.daemon")


class Daemon:
    def __init__(self) -> None:
        self.state = State()
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
        self.mode = "trigger"
        self.voice_status: dict[str, Any] = {"state": "idle"}
        self._audio_capture: AudioCapture | None = None
        self._loop: asyncio.AbstractEventLoop | None = None

    # ─── BLE plumbing ──────────────────────────────────────────

    async def _on_ble_connect(self) -> None:
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

        if cmd == "key_tap":
            from .keyinject import key_tap
            key_tap(msg.get("mod"), msg.get("key", ""))
            return
        if cmd == "key_down":
            key_down(msg.get("mod"), msg.get("key", ""))
            return
        if cmd == "key_up":
            key_up(msg.get("mod"), msg.get("key", ""))
            return

        if cmd == "btn":
            self._handle_button(msg.get("key", ""), msg.get("edge", ""))
            return

        if cmd in ("name", "owner", "unpair"):
            await self.ble.send(generic_ack(cmd, ok=True))
            return

        log.debug("unhandled device message: %s", msg)

    def _handle_button(self, key: str, edge: str) -> None:
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
            log.warning("mic mode not implemented; ignoring btn %s", edge)
            return

        raise ValueError(f"unknown mode: {self.mode!r}")

    # ─── audio ─────────────────────────────────────────────────

    def _on_audio_frame(self, raw: bytes) -> None:
        try:
            if raw and raw[0] == TYPE_STREAM_START:
                self._audio_capture = AudioCapture()
            if self._audio_capture is None:
                return
            self._audio_capture.feed(raw)
            if self._audio_capture.ended:
                capture, self._audio_capture = self._audio_capture, None
                if self._loop is not None:
                    self._loop.call_soon_threadsafe(self._dump_audio, capture)
        except AudioFrameError as e:
            log.error("audio frame error, dropping stream: %s", e)
            self._audio_capture = None

    def _dump_audio(self, capture: AudioCapture) -> None:
        ts = time.strftime("%Y%m%d-%H%M%S")
        wav_path = DEBUG_WAV_DIR / f"ptt-{ts}.wav"
        try:
            finish_and_dump(capture, wav_path)
        except Exception as e:
            log.error("audio dump failed: %s", e)

    # ─── heartbeat ─────────────────────────────────────────────

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

    # ─── orchestration ─────────────────────────────────────────

    async def run(self) -> None:
        app = web.Application()
        register_routes(app, self)
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

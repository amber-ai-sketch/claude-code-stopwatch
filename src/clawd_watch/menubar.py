"""Menu-bar app for clawd-watch — see and control the BLE link to the watch.

A thin front-end over the daemon's localhost HTTP API. It never touches BLE
itself (the daemon owns that single link), so it needs no Bluetooth permission
and can't fight the daemon for the radio. It polls /status every few seconds to
keep the title icon honest, and the menu items map to /scan, /reconnect,
/forget, /connect and a launchd restart.

Title icon:  🟢 connected   🔴 disconnected   🟡 scanning   ⚪ daemon down
"""
from __future__ import annotations

import os
import subprocess
import sys
import threading
import urllib.error

import rumps

from . import LOG_PATH
from .http_client import daemon_get, daemon_post

PLIST_LABEL = "com.claude-code.clawd-watch"
POLL_SECONDS = 3


class ClawdWatchMenuBar(rumps.App):
    def __init__(self) -> None:
        super().__init__("Clawd", title="⚪ Clawd", quit_button="退出")

        # Status rows (read-only, refreshed each poll).
        self._link_row = rumps.MenuItem("启动中…")
        self._addr_row = rumps.MenuItem("")
        self._info_row = rumps.MenuItem("")   # model + cost, combined
        self._ctx_row = rumps.MenuItem("")    # context % + rate limits

        # Mode toggle.
        self._mode_trigger = rumps.MenuItem("遥控器", callback=self._on_mode_trigger)
        self._mode_mic = rumps.MenuItem("录音", callback=self._on_mode_mic)
        self._mode_menu = rumps.MenuItem("模式")
        self._mode_menu.add(self._mode_trigger)
        self._mode_menu.add(self._mode_mic)

        # Nearby devices submenu.
        self._nearby = rumps.MenuItem("附近设备")

        self.menu = [
            self._link_row,
            self._addr_row,
            None,
            self._mode_menu,
            None,
            self._info_row,
            self._ctx_row,
            None,
            rumps.MenuItem("扫描附近设备…", callback=self._on_scan),
            self._nearby,
            rumps.MenuItem("重新连接", callback=self._on_reconnect),
            rumps.MenuItem("忘记此设备", callback=self._on_forget),
            rumps.MenuItem("重启后台服务", callback=self._on_restart),
            rumps.MenuItem("查看日志", callback=self._on_logs),
        ]

        # Set by the scan worker thread, consumed on the next poll tick.
        self._scan_result: list | None = None
        self._scan_done = False  # explicit handoff flag (thread-safe pattern)

        rumps.Timer(self._poll, POLL_SECONDS).start()

    # ─── periodic refresh (main thread) ────────────────────────

    def _poll(self, _timer) -> None:
        if self._scan_result is not None:
            self._render_nearby(self._scan_result)
            self._scan_result = None

        try:
            status = daemon_get("/status", timeout=2.0)
        except (urllib.error.URLError, OSError):
            self.title = "⚪ Clawd"
            self._link_row.title = "服务未运行"
            self._addr_row.title = "选择「重启后台服务」"
            self._info_row.title = "—"
            self._ctx_row.title = ""
            return

        connected = status.get("connected", False)
        if self._scan_done:
            self.title = "🟡 Clawd"
            self._scan_done = False
        else:
            self.title = "🟢 Clawd" if connected else "🔴 Clawd"

        if connected:
            self._link_row.title = f"● 已连接  {status.get('device_name') or '设备'}"
        else:
            self._link_row.title = "○ 未连接 — 重试中…"

        address = status.get("address")
        self._addr_row.title = f"地址 …{address[-8:]}" if address else "未保存设备"

        # Combined info line: "model $cost" or just "model" or "—".
        sl = status.get("statusline") or {}
        parts = []
        if sl.get("model_name"):
            parts.append(sl["model_name"])
        if sl.get("cost_usd") is not None:
            parts.append(f"${sl['cost_usd']:.2f}")
        self._info_row.title = "  ".join(parts) if parts else "—"

        # Context + rate limits on one line.
        ctx_parts = []
        ctx = sl.get("context_pct")
        if ctx is not None:
            ctx_parts.append(f"上下文 {ctx:.0f}%")
        r5h = sl.get("rate_5h_pct")
        if r5h is not None:
            ctx_parts.append(f"5h {r5h:.0f}%")
        self._ctx_row.title = "  ·  ".join(ctx_parts) if ctx_parts else ""

        mode = status.get("mode", "trigger")
        self._mode_trigger.state = 1 if mode == "trigger" else 0
        self._mode_mic.state = 1 if mode == "mic" else 0

    # ─── menu actions ──────────────────────────────────────────

    def _on_mode_trigger(self, _sender) -> None:
        self._set_mode("trigger")

    def _on_mode_mic(self, _sender) -> None:
        self._set_mode("mic")

    def _set_mode(self, mode: str) -> None:
        try:
            daemon_post("/mode", {"mode": mode}, timeout=2.0)
        except (urllib.error.URLError, OSError) as e:
            rumps.alert("切换模式失败", str(e))
            return
        self._mode_trigger.state = 1 if mode == "trigger" else 0
        self._mode_mic.state = 1 if mode == "mic" else 0

    def _on_scan(self, _sender) -> None:
        if self._scan_done:
            return
        self._scan_done = True
        self._nearby.title = "附近设备（扫描中…）"
        threading.Thread(target=self._scan_worker, daemon=True).start()

    def _scan_worker(self) -> None:
        try:
            result = daemon_get("/scan", timeout=12.0).get("devices", [])
        except (urllib.error.URLError, OSError) as e:
            result = []
            print(f"scan failed: {e}", file=sys.stderr)
        self._scan_result = result

    def _render_nearby(self, devices: list) -> None:
        self._nearby.clear()
        if not devices:
            self._nearby.title = "附近设备（无）"
            return
        self._nearby.title = f"附近设备（{len(devices)}）"
        try:
            saved = (daemon_get("/status", timeout=2.0) or {}).get("address")
        except (urllib.error.URLError, OSError):
            saved = None
        for d in devices:
            address = d["address"]
            rssi = f"{d['rssi']}dBm" if d.get("rssi") is not None else "?"
            star = "★ " if address == saved else ""
            label = f"{star}{rssi:>7}  …{address[-8:]}  {d['name'] or ''}".rstrip()
            self._nearby.add(
                rumps.MenuItem(label, callback=self._make_connect(address))
            )

    def _make_connect(self, address: str):
        def _connect(_sender) -> None:
            try:
                daemon_post("/connect", {"address": address}, timeout=5.0)
            except (urllib.error.URLError, OSError) as e:
                print(f"connect failed: {e}", file=sys.stderr)
        return _connect

    def _on_reconnect(self, _sender) -> None:
        try:
            daemon_post("/reconnect", {}, timeout=5.0)
        except (urllib.error.URLError, OSError) as e:
            print(f"reconnect failed: {e}", file=sys.stderr)

    def _on_forget(self, _sender) -> None:
        if not rumps.alert("忘记此设备？", "下次将重新扫描配对。", ok="忘记", cancel="取消"):
            return
        try:
            daemon_post("/forget", {}, timeout=5.0)
            self._nearby.clear()
            self._nearby.title = "附近设备"
        except (urllib.error.URLError, OSError) as e:
            print(f"forget failed: {e}", file=sys.stderr)

    def _on_restart(self, _sender) -> None:
        r = subprocess.run(
            ["launchctl", "kickstart", "-k", f"gui/{os.getuid()}/{PLIST_LABEL}"],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            print(f"restart failed: {r.stderr.strip() or r.returncode}", file=sys.stderr)

    def _on_logs(self, _sender) -> None:
        subprocess.Popen(["open", LOG_PATH])


def main() -> None:
    ClawdWatchMenuBar().run()


if __name__ == "__main__":
    main()

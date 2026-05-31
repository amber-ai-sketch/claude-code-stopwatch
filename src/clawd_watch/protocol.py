"""BLE wire protocol for clawd-watch.

Same Nordic UART Service UUIDs as claude-desktop-buddy so any BLE library
that targets that protocol still works against this device. Heartbeat
JSON adds statusline-derived fields (cost, ctx, r5h, r7d, model). Compact
key names + 2-decimal rounding to keep payload under the 247-byte default
BLE MTU after the JSON envelope.
"""
from __future__ import annotations

import time
from typing import Any, Optional

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host → device, write
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → host, notify

# Dedicated audio service: binary PCM frames, NOT the newline-JSON channel.
# Must match firmware ble_nus.cpp AUDIO_SVC_UUID / AUDIO_TX_UUID.
AUDIO_SERVICE = "6e40a000-b5a3-f393-e0a9-e50e24dcca9e"
AUDIO_TX = "6e40a003-b5a3-f393-e0a9-e50e24dcca9e"  # device → host, notify


def time_sync_msg() -> dict[str, Any]:
    # [epoch_seconds, timezone_offset_in_seconds_east_of_utc]
    # time.timezone is seconds WEST of UTC, so negate it.
    return {"time": [int(time.time()), -time.timezone]}


def owner_msg(name: str) -> dict[str, Any]:
    return {"cmd": "owner", "name": name}


def heartbeat(
    *,
    total: int,
    running: int,
    waiting: int,
    msg: str,
    prompt: Optional[dict[str, Any]] = None,
    # Statusline-derived fields. Firmware ignores any it doesn't know.
    cost_usd: Optional[float] = None,
    context_pct: Optional[float] = None,
    rate_5h_pct: Optional[float] = None,
    rate_7d_pct: Optional[float] = None,
    model_name: Optional[str] = None,
    # Tool currently running (busy.bash / busy.edit / busy.web ...). Firmware
    # uses this to pick which motion graphic to render. Cleared on Stop hook.
    current_tool: Optional[str] = None,
    # Per-session detail array for the device's swipeable detail pages.
    # Each item: {sid, run, wait, ctx, cost, model, tool, proj, tin, tout,
    # cr, cw}. Firmware ignores if absent. Sent on its own line when present
    # to respect the BLE MTU.
    sessions: Optional[list[dict[str, Any]]] = None,
) -> dict[str, Any]:
    out: dict[str, Any] = {
        "total": total,
        "running": running,
        "waiting": waiting,
        "msg": msg,
    }
    if prompt is not None:
        out["prompt"] = prompt
    if sessions is not None:
        out["sessions"] = sessions
    # Compact key names + rounding shrink BLE payload.
    if cost_usd is not None:
        out["cost"] = round(cost_usd, 2)
    if context_pct is not None:
        out["ctx"] = round(context_pct, 1)
    if rate_5h_pct is not None:
        out["r5h"] = round(rate_5h_pct, 1)
    if rate_7d_pct is not None:
        out["r7d"] = round(rate_7d_pct, 1)
    if model_name is not None:
        out["model"] = model_name
    if current_tool is not None:
        out["tool"] = current_tool
    return out


def status_ack(
    *,
    name: str = "clawd-watch",
    secure: bool = False,
    approvals: int = 0,
    denials: int = 0,
    up_seconds: int = 0,
) -> dict[str, Any]:
    return {
        "ack": "status",
        "ok": True,
        "data": {
            "name": name,
            "sec": secure,
            "sys": {"up": up_seconds, "heap": 0},
            "stats": {"appr": approvals, "deny": denials},
        },
    }


def generic_ack(cmd: str, ok: bool = True, n: int = 0) -> dict[str, Any]:
    return {"ack": cmd, "ok": ok, "n": n}


def short_hint_for_tool(tool: str, tool_input: dict[str, Any]) -> str:
    """One-line hint for the device's attention page, ≤120 chars."""
    if not isinstance(tool_input, dict):
        return ""
    if tool == "Bash":
        lines = (tool_input.get("command") or "").strip().splitlines()
        return lines[0][:120] if lines else ""
    if tool in ("Write", "Edit", "MultiEdit", "NotebookEdit"):
        return (tool_input.get("file_path") or tool_input.get("notebook_path") or "")[:120]
    if tool == "WebFetch":
        return (tool_input.get("url") or "")[:120]
    if tool == "WebSearch":
        return (tool_input.get("query") or "")[:120]
    for v in tool_input.values():
        if isinstance(v, str):
            return v[:120]
    return ""

"""User-facing CLI: clawd-watch {status|test|tail|restart|install-hooks|uninstall-hooks}."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

from . import HTTP_HOST, HTTP_PORT, LOG_PATH


PLIST_LABEL = "com.claude-code.clawd-watch"


def _get(path: str, timeout: float = 5.0) -> dict:
    req = urllib.request.Request(f"http://{HTTP_HOST}:{HTTP_PORT}{path}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def _post(path: str, payload: dict | None = None, timeout: float = 35.0) -> dict:
    data = json.dumps(payload or {}).encode()
    req = urllib.request.Request(
        f"http://{HTTP_HOST}:{HTTP_PORT}{path}",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def cmd_status(_args: argparse.Namespace) -> int:
    try:
        info = _get("/status")
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1

    conn = "✓ connected" if info["connected"] else "✗ disconnected"
    print(f"daemon:  uptime {info['uptime_s']}s")
    print(f"ble:     {conn}  device={info.get('device_name')}  address={info.get('address')}")
    print(f"stats:   approvals={info['approvals_total']} denials={info['denials_total']}")

    sl = info.get("statusline") or {}
    if sl:
        bits = []
        if "model_name" in sl: bits.append(f"model={sl['model_name']}")
        if "cost_usd" in sl: bits.append(f"cost=${sl['cost_usd']:.2f}")
        if "context_pct" in sl: bits.append(f"ctx={sl['context_pct']:.0f}%")
        if "rate_5h_pct" in sl: bits.append(f"5h={sl['rate_5h_pct']:.0f}%")
        if "rate_7d_pct" in sl: bits.append(f"7d={sl['rate_7d_pct']:.0f}%")
        print(f"line:    {' '.join(bits) if bits else '(empty)'}")
    if info.get("current_tool"):
        print(f"tool:    {info['current_tool']}")
    voice = info.get("voice") or {}
    if voice:
        print(f"voice:   {voice.get('state', 'unknown')}")
        last_test = voice.get("last_test") or {}
        if last_test:
            print(
                f"         last_test ok={last_test.get('ok')} "
                f"device={last_test.get('device_index')} rc={last_test.get('returncode')}"
            )

    sessions = info["sessions"]
    print(f"sessions ({len(sessions)}):")
    for s in sessions:
        flag = "▶ running" if s["running"] else "· idle"
        print(f"  {flag}  {s['session_id']}")
    pending = info["pending"]
    print(f"pending approvals ({len(pending)}):")
    for p in pending:
        print(f"  {p['tool']:<10}  {p['hint'][:80]}  (id {p['id']})")
    return 0


def cmd_test(_args: argparse.Namespace) -> int:
    print("Injecting a test approval. Press LEFT on the watch to allow, RIGHT to deny.")
    try:
        out = _post("/test-prompt", {}, timeout=40.0)
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    print(f"result: {out['decision']}  id={out['id']}")
    return 0


def cmd_test_statusline(_args: argparse.Namespace) -> int:
    """Push a fake statusline payload to /statusline. Useful before firmware exists."""
    fake = {
        "cost_usd": 1.23,
        "context_pct": 42.5,
        "rate_5h_pct": 18.0,
        "rate_7d_pct": 35.0,
        "model_name": "Opus 4.7",
    }
    try:
        out = _post("/statusline", fake, timeout=2.0)
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    print(f"posted: {fake}")
    print(f"reply:  {out}")
    return 0


def cmd_voice_test(args: argparse.Namespace) -> int:
    payload = {"text": args.text} if args.text else {}
    if args.device_index is not None:
        payload["device_index"] = args.device_index
    try:
        out = _post("/voice-test", payload, timeout=45.0)
    except urllib.error.HTTPError as e:
        print(f"voice-test rejected: {e.reason}")
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    state = "ok" if out.get("ok") else "failed"
    print(f"voice-test: {state} device={out.get('device_index')} rc={out.get('returncode')}")
    if out.get("stderr"):
        print(out["stderr"])
    return 0 if out.get("ok") else 1


def cmd_scan(_args: argparse.Namespace) -> int:
    try:
        out = _get("/scan", timeout=12.0)
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    devices = out.get("devices", [])
    if not devices:
        print("no devices found (is the watch awake? is Bluetooth permission granted?)")
        return 0
    print(f"found {len(devices)} device(s):")
    for d in devices:
        rssi = f"{d['rssi']}dBm" if d.get("rssi") is not None else "?"
        print(f"  {rssi:>7}  {d['address']}  {d['name'] or '(unnamed)'}")
    return 0


def cmd_reconnect(_args: argparse.Namespace) -> int:
    try:
        out = _post("/reconnect", {}, timeout=5.0)
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    print(f"reconnecting to {out.get('address') or '(scan)'}")
    return 0


def cmd_forget(_args: argparse.Namespace) -> int:
    try:
        _post("/forget", {}, timeout=5.0)
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    print("forgot device; daemon will scan by name")
    return 0


def cmd_connect(args: argparse.Namespace) -> int:
    try:
        out = _post("/connect", {"address": args.address}, timeout=5.0)
    except urllib.error.HTTPError as e:
        print(f"connect rejected: {e.reason}")
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"daemon: not reachable ({e})")
        return 1
    print(f"connecting to {out.get('address')}")
    return 0


def cmd_tail(_args: argparse.Namespace) -> int:
    if not os.path.exists(LOG_PATH):
        print(f"log file not found: {LOG_PATH}")
        return 1
    os.execvp("tail", ["tail", "-f", LOG_PATH])


def cmd_restart(_args: argparse.Namespace) -> int:
    r = subprocess.run(
        ["launchctl", "kickstart", "-k", f"gui/{os.getuid()}/{PLIST_LABEL}"],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return r.returncode
    print("restarted")
    return 0


def cmd_install_hooks(args: argparse.Namespace) -> int:
    """Merge our hooks + statusLine config into ~/.claude/settings.json."""
    from pathlib import Path

    settings_path = Path(args.settings)
    hook_bin = args.hook_bin
    statusline_bin = args.statusline_bin

    desired_hooks = {
        "SessionStart": [
            {"hooks": [{"type": "command", "command": f"{hook_bin} SessionStart"}]}
        ],
        "SessionEnd": [
            {"hooks": [{"type": "command", "command": f"{hook_bin} SessionEnd"}]}
        ],
        "UserPromptSubmit": [
            {"hooks": [{"type": "command", "command": f"{hook_bin} UserPromptSubmit"}]}
        ],
        "Stop": [
            {"hooks": [{"type": "command", "command": f"{hook_bin} Stop"}]}
        ],
        "PreToolUse": [
            {
                "matcher": "Bash|Write|Edit|MultiEdit|NotebookEdit",
                "hooks": [
                    {
                        "type": "command",
                        "command": f"{hook_bin} PreToolUse",
                        "timeout": 35,
                    }
                ],
            }
        ],
        "PostToolUse": [
            {"hooks": [{"type": "command", "command": f"{hook_bin} PostToolUse"}]}
        ],
    }

    if settings_path.exists():
        try:
            existing = json.loads(settings_path.read_text())
        except Exception as e:
            print(f"could not parse {settings_path}: {e}", file=sys.stderr)
            return 1
    else:
        existing = {}

    # 1) merge hooks
    hooks = existing.setdefault("hooks", {})
    added: list[str] = []
    skipped: list[str] = []
    for event, entries in desired_hooks.items():
        bucket = hooks.setdefault(event, [])
        installed = any(
            any(h.get("command", "").startswith(hook_bin) for h in entry.get("hooks", []))
            for entry in bucket
            if isinstance(entry, dict)
        )
        if installed:
            skipped.append(event)
            continue
        bucket.extend(entries)
        added.append(event)

    # 2) merge statusLine. Format per https://code.claude.com/docs/en/statusline:
    #    settings.json supports either a string, or {type: "command", command: "..."}.
    statusline_added = False
    statusline_skipped = False
    sl = existing.get("statusLine")
    desired_sl = {"type": "command", "command": statusline_bin}
    if isinstance(sl, dict) and sl.get("command", "").startswith(statusline_bin):
        statusline_skipped = True
    elif isinstance(sl, str) and sl.startswith(statusline_bin):
        statusline_skipped = True
    elif sl is None or sl == "":
        existing["statusLine"] = desired_sl
        statusline_added = True
    else:
        # User has a different statusLine. Don't clobber — warn instead.
        print(
            f"warning: existing statusLine ({sl!r}) not overwritten. "
            f"To enable clawd-watch metrics on the device, set:\n"
            f"  statusLine: {desired_sl}\n"
            f"in {settings_path} (or remove the existing entry and re-run install).",
            file=sys.stderr,
        )

    tmp = settings_path.with_suffix(settings_path.suffix + ".tmp")
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_text(json.dumps(existing, indent=2))
    tmp.replace(settings_path)

    if added:
        print(f"added hooks for: {', '.join(added)}")
    if skipped:
        print(f"already installed: {', '.join(skipped)}")
    if statusline_added:
        print(f"added statusLine -> {statusline_bin}")
    if statusline_skipped:
        print(f"statusLine already points at {statusline_bin}")
    if not added and not skipped and not statusline_added:
        print("no changes")
    return 0


def cmd_uninstall_hooks(args: argparse.Namespace) -> int:
    from pathlib import Path

    settings_path = Path(args.settings)
    hook_bin = args.hook_bin
    statusline_bin = args.statusline_bin
    if not settings_path.exists():
        print("no settings.json; nothing to do")
        return 0
    data = json.loads(settings_path.read_text())

    # remove hooks
    hooks = data.get("hooks", {})
    removed = 0
    for event, bucket in list(hooks.items()):
        new_bucket = []
        for entry in bucket:
            if not isinstance(entry, dict):
                new_bucket.append(entry)
                continue
            inner = [
                h for h in entry.get("hooks", [])
                if not h.get("command", "").startswith(hook_bin)
            ]
            if inner:
                entry = dict(entry)
                entry["hooks"] = inner
                new_bucket.append(entry)
            else:
                removed += 1
        if new_bucket:
            hooks[event] = new_bucket
        else:
            hooks.pop(event, None)
    if not hooks:
        data.pop("hooks", None)

    # remove statusLine if it's ours
    sl = data.get("statusLine")
    if isinstance(sl, dict) and sl.get("command", "").startswith(statusline_bin):
        data.pop("statusLine", None)
        print("removed statusLine")
    elif isinstance(sl, str) and sl.startswith(statusline_bin):
        data.pop("statusLine", None)
        print("removed statusLine")

    settings_path.write_text(json.dumps(data, indent=2))
    print(f"removed {removed} hook entr{'y' if removed == 1 else 'ies'}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="clawd-watch")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="show daemon + BLE + session summary").set_defaults(
        func=cmd_status
    )
    sub.add_parser("test", help="inject a test approval prompt").set_defaults(
        func=cmd_test
    )
    sub.add_parser("test-statusline", help="push a fake statusline payload (no Claude Code needed)").set_defaults(
        func=cmd_test_statusline
    )
    voice = sub.add_parser("voice-test", help="play spoken test audio into BlackHole")
    voice.add_argument("--text", help="text to synthesize and play")
    voice.add_argument("--device-index", type=int, help="ffmpeg AudioToolbox output index")
    voice.set_defaults(func=cmd_voice_test)
    sub.add_parser("scan", help="scan for nearby BLE devices").set_defaults(
        func=cmd_scan
    )
    sub.add_parser("reconnect", help="force an immediate BLE reconnect").set_defaults(
        func=cmd_reconnect
    )
    sub.add_parser("forget", help="forget the saved device; scan by name again").set_defaults(
        func=cmd_forget
    )
    conn = sub.add_parser("connect", help="connect to a specific BLE address and remember it")
    conn.add_argument("address")
    conn.set_defaults(func=cmd_connect)
    sub.add_parser("tail", help="tail -f the daemon log").set_defaults(func=cmd_tail)
    sub.add_parser("restart", help="restart the launchd service").set_defaults(
        func=cmd_restart
    )

    home = os.path.expanduser("~")
    default_settings = f"{home}/.claude/settings.json"
    default_hook = f"{home}/.local/bin/clawd-watch-hook"
    default_statusline = f"{home}/.local/bin/clawd-statusline"

    inst = sub.add_parser(
        "install-hooks",
        help="merge hooks + statusLine into ~/.claude/settings.json",
    )
    inst.add_argument("--settings", default=default_settings)
    inst.add_argument("--hook-bin", default=default_hook)
    inst.add_argument("--statusline-bin", default=default_statusline)
    inst.set_defaults(func=cmd_install_hooks)

    uninst = sub.add_parser(
        "uninstall-hooks",
        help="remove our hooks + statusLine from ~/.claude/settings.json",
    )
    uninst.add_argument("--settings", default=default_settings)
    uninst.add_argument("--hook-bin", default=default_hook)
    uninst.add_argument("--statusline-bin", default=default_statusline)
    uninst.set_defaults(func=cmd_uninstall_hooks)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

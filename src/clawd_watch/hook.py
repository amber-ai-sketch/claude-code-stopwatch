"""Claude Code hook entry point for clawd-watch.

Invoked as: clawd-watch-hook <EventName>. Reads hook payload from stdin,
POSTs it to the daemon. For PreToolUse, prints hookSpecificOutput JSON
back to stdout so Claude Code applies the decision the watch user picked.

If the daemon is unreachable, PreToolUse falls back to permissionDecision=ask
so Claude Code's normal terminal prompt handles approval. Other events
silently exit 0.
"""
from __future__ import annotations

import json
import sys
import urllib.error

from . import HOOK_REQUEST_TIMEOUT
from .http_client import daemon_post


def _post(event: str, payload: dict) -> dict:
    return daemon_post(f"/{event}", payload, timeout=HOOK_REQUEST_TIMEOUT)


def _emit_pretool_decision(decision: str, reason: str) -> None:
    sys.stdout.write(
        json.dumps(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "permissionDecision": decision,
                    "permissionDecisionReason": reason,
                }
            }
        )
    )


def main() -> int:
    event = sys.argv[1] if len(sys.argv) > 1 else ""
    if not event:
        sys.stderr.write("clawd-watch-hook: missing event name\n")
        return 0  # don't block Claude Code

    try:
        payload_raw = sys.stdin.read()
        payload = json.loads(payload_raw) if payload_raw else {}
    except Exception as e:
        sys.stderr.write(f"clawd-watch-hook: bad stdin json: {e}\n")
        payload = {}

    # Bypass mode: --dangerously-skip-permissions. Don't bounce decisions
    # off the device.
    if event == "PreToolUse" and payload.get("permission_mode") == "bypassPermissions":
        _emit_pretool_decision(
            "allow", "watch skipped: --dangerously-skip-permissions"
        )
        return 0

    try:
        body = _post(event, payload)
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        if event == "PreToolUse":
            _emit_pretool_decision("ask", f"watch unavailable: {e}")
        return 0

    if event == "PreToolUse":
        decision = body.get("decision", "ask")
        reason = body.get("reason", "watch decision")
        if decision not in ("allow", "deny", "ask"):
            decision = "ask"
        _emit_pretool_decision(decision, reason)

    return 0


if __name__ == "__main__":
    sys.exit(main())

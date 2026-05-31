"""statusLine wrapper that runs both ccusage AND clawd-statusline.

User has an existing ccusage-based statusLine they want to keep. This
wrapper:
  - tees the stdin JSON to both ccusage (visual) and clawd-statusline
    (BLE forwarder)
  - prints whatever ccusage prints (preserves user's existing visual)
  - runs the BLE forwarder in parallel with a tight deadline so it can't
    slow the CLI down

Configured via ~/.claude/settings.json:
  statusLine.command = "~/.local/bin/clawd-statusline-tee"

We don't shell out to bash / pipes here — the parent statusLine already
has its sed filter chained, so this wrapper drops INTO that chain. To
preserve the user's exact existing pipeline, we don't try to recreate
the sed filter. Instead we accept JSON on stdin, fire-and-forget the
POST in a thread, then run the user's ORIGINAL command verbatim and
forward its stdout.
"""
from __future__ import annotations

import glob
import os
import subprocess
import sys
import threading

from . import HTTP_HOST, HTTP_PORT
from .statusline import _normalize, _post_to_daemon, _format_status_line

_SED_FILTER = "sed -E 's# / \\$[0-9.]+ block \\([^)]+\\)##; s# \\| 🔥 \\$[0-9.]+/hr##'"


def _ccusage_cmd() -> str:
    """Return the fastest available way to run 'ccusage statusline'.

    Prefers a direct 'bun <cached-cli.js>' invocation (no dep resolution)
    over 'bunx ccusage' which re-resolves on every call and can exceed the
    2.5s timeout.
    """
    bun_cache = os.path.expanduser("~/.bun/install/cache")
    pattern = os.path.join(bun_cache, "ccusage@*/dist/cli.js")
    candidates = sorted(glob.glob(pattern), reverse=True)
    if candidates:
        cli = candidates[0]
        return f"bun {cli} statusline | {_SED_FILTER}"
    return f"bunx ccusage statusline | {_SED_FILTER}"


INNER_CMD = os.environ.get("CLAWD_STATUSLINE_INNER") or _ccusage_cmd()


def _patch_for_ccusage(payload: dict) -> dict:
    """Inject fields ccusage requires but proxy APIs omit."""
    import copy
    p = copy.deepcopy(payload)
    ctx = p.get("context_window")
    if isinstance(ctx, dict) and "total_input_tokens" not in ctx:
        # Reconstruct token count from used_percentage + context_window_size.
        # ccusage only needs a plausible number; 0 is valid per its schema.
        size = ctx.get("context_window_size", 200_000)
        pct = ctx.get("used_percentage", 0)
        ctx["total_input_tokens"] = int(size * pct / 100)
        ctx.setdefault("context_window_size", 200_000)
    return p


def _parse_and_patch(raw: bytes) -> tuple[dict, bytes]:
    """Parse raw JSON, patch missing fields, return (norm, patched_bytes)."""
    import json
    try:
        payload = json.loads(raw or b"{}")
    except Exception:
        return {}, raw
    if not isinstance(payload, dict):
        return {}, raw
    norm = _normalize(payload)
    patched = json.dumps(_patch_for_ccusage(payload)).encode()
    return norm, patched


def main() -> int:
    raw = sys.stdin.buffer.read()
    norm, patched = _parse_and_patch(raw)

    # Fire-and-forget BLE POST.
    def _post():
        if norm:
            _post_to_daemon(norm)
    bg = threading.Thread(target=_post, daemon=True)
    bg.start()

    # Feed patched JSON to ccusage so missing fields don't cause parse errors.
    output = b""
    try:
        proc = subprocess.run(
            INNER_CMD,
            shell=True,
            input=patched,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=5.0,
        )
        output = proc.stdout
    except subprocess.TimeoutExpired:
        print("clawd-statusline: ccusage timed out after 5s", file=sys.stderr)
    except Exception as e:
        print(f"clawd-statusline: ccusage failed: {e}", file=sys.stderr)

    if output and output.strip():
        sys.stdout.buffer.write(output)
    else:
        sys.stdout.write(_format_status_line(norm))
        sys.stdout.write("\n")

    # Briefly join the background thread so the process doesn't exit
    # before urllib's connect attempt finishes. 0.3s mirrors the urlopen
    # timeout in _post_to_daemon.
    bg.join(timeout=0.35)
    return 0


if __name__ == "__main__":
    sys.exit(main())

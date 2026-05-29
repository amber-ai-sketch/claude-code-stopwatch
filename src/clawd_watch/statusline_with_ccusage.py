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

import os
import subprocess
import sys
import threading

from . import HTTP_HOST, HTTP_PORT
from .statusline import _normalize, _post_to_daemon

# What to run for the visual statusline. Override via env if you want to
# tweak. The default mirrors the user's settings.json line at install
# time. The shell allows the trailing sed filter to survive.
DEFAULT_INNER = (
    "bunx ccusage statusline | "
    "sed -E 's# / \\$[0-9.]+ block \\([^)]+\\)##; s# \\| 🔥 \\$[0-9.]+/hr##'"
)
INNER_CMD = os.environ.get("CLAWD_STATUSLINE_INNER", DEFAULT_INNER)


def _post_in_background(raw: bytes) -> threading.Thread:
    """Fire-and-forget thread: parse + POST. Bounded by urlopen 0.3s."""
    def _run():
        import json
        try:
            payload = json.loads(raw or b"{}")
        except Exception:
            return
        norm = _normalize(payload) if isinstance(payload, dict) else {}
        if norm:
            _post_to_daemon(norm)

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t


def main() -> int:
    raw = sys.stdin.buffer.read()

    # Kick off the BLE-side post BEFORE running ccusage so they overlap.
    bg = _post_in_background(raw)

    # Run the user's original statusLine command, feeding it the same
    # stdin bytes. Output its stdout 1:1 so the visual is unchanged.
    try:
        proc = subprocess.run(
            INNER_CMD,
            shell=True,
            input=raw,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=2.5,
        )
        sys.stdout.buffer.write(proc.stdout)
    except subprocess.TimeoutExpired:
        # ccusage hung — print a fallback so the CLI bar isn't empty.
        sys.stdout.write("watch")
    except Exception:
        sys.stdout.write("watch")

    # Briefly join the background thread so the process doesn't exit
    # before urllib's connect attempt finishes. 0.3s mirrors the urlopen
    # timeout in _post_to_daemon.
    bg.join(timeout=0.35)
    return 0


if __name__ == "__main__":
    sys.exit(main())

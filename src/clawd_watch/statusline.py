"""Claude Code statusline shim — pushes session metrics to the watch.

Wired up by ~/.claude/settings.json's `statusLine.command`. Runs on every
Claude Code statusline tick. Reads Claude Code's statusline JSON from
stdin, posts the fields we care about to the local daemon, and prints a
one-line status to stdout so Claude Code's CLI bar still has something
to show.

Performance: the HTTP POST has a 0.3s timeout and any failure is silent.
Daemon down must NOT degrade the CLI experience.

Field mapping (Claude Code statusline JSON → daemon):
  model.display_name                                  → model_name
  cost.total_cost_usd                                 → cost_usd
  context_window.used_percentage                      → context_pct
  context_window.current_usage.input_tokens           → input_tokens
  context_window.current_usage.output_tokens          → output_tokens
  context_window.current_usage.cache_read_input_tokens     → cache_read_tokens
  context_window.current_usage.cache_creation_input_tokens → cache_create_tokens
  workspace.current_dir (basename)                    → project
  rate_limits.five_hour.used_percentage               → rate_5h_pct
  rate_limits.seven_day.used_percentage               → rate_7d_pct

Schema reference: https://code.claude.com/docs/en/statusline
"""
from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request

from . import HTTP_HOST, HTTP_PORT


def normalize(payload: dict) -> dict:
    """Pull keys we care about out of a Claude Code statusline JSON."""
    out: dict = {}

    sid = payload.get("session_id")
    if isinstance(sid, str) and sid:
        out["session_id"] = sid

    model = payload.get("model")
    if isinstance(model, dict):
        name = model.get("display_name") or model.get("id")
        if name:
            out["model_name"] = name

    cost = payload.get("cost")
    if isinstance(cost, dict):
        c = cost.get("total_cost_usd")
        if isinstance(c, (int, float)):
            out["cost_usd"] = float(c)

    ctx = payload.get("context_window")
    if isinstance(ctx, dict):
        pct = ctx.get("used_percentage")
        if isinstance(pct, (int, float)):
            out["context_pct"] = float(pct)

        usage = ctx.get("current_usage")
        if isinstance(usage, dict):
            for src, dst in (
                ("input_tokens", "input_tokens"),
                ("output_tokens", "output_tokens"),
                ("cache_read_input_tokens", "cache_read_tokens"),
                ("cache_creation_input_tokens", "cache_create_tokens"),
            ):
                v = usage.get(src)
                if isinstance(v, int):
                    out[dst] = v

    # Session name — user-set via --name or /rename, highest priority title.
    session_name = payload.get("session_name")
    if isinstance(session_name, str) and session_name:
        out["session_name"] = session_name

    workspace = payload.get("workspace")
    if isinstance(workspace, dict):
        cwd = workspace.get("current_dir")
        if isinstance(cwd, str) and cwd:
            out["project"] = os.path.basename(cwd.rstrip("/"))

    # Worktree name — if running in a git worktree.
    worktree = payload.get("worktree")
    if isinstance(worktree, dict):
        wt_name = worktree.get("name")
        if isinstance(wt_name, str) and wt_name:
            out["worktree_name"] = wt_name

    # Agent name — if running with --agent flag.
    agent = payload.get("agent")
    if isinstance(agent, dict):
        agent_name = agent.get("name")
        if isinstance(agent_name, str) and agent_name:
            out["agent_name"] = agent_name

    rl = payload.get("rate_limits")
    if isinstance(rl, dict):
        five = rl.get("five_hour")
        if isinstance(five, dict):
            v = five.get("used_percentage")
            if isinstance(v, (int, float)):
                out["rate_5h_pct"] = float(v)
        seven = rl.get("seven_day")
        if isinstance(seven, dict):
            v = seven.get("used_percentage")
            if isinstance(v, (int, float)):
                out["rate_7d_pct"] = float(v)

    return out


def format_status_line(norm: dict) -> str:
    parts: list[str] = []
    if "model_name" in norm:
        parts.append(str(norm["model_name"]))
    if "cost_usd" in norm:
        parts.append(f"${norm['cost_usd']:.2f}")
    if "context_pct" in norm:
        parts.append(f"ctx {norm['context_pct']:.0f}%")
    if "rate_5h_pct" in norm:
        parts.append(f"5h {norm['rate_5h_pct']:.0f}%")
    if not parts:
        return "watch"
    return " · ".join(parts)


def post_to_daemon(norm: dict) -> None:
    """Fire-and-forget POST. Never raises."""
    try:
        data = json.dumps(norm).encode()
        req = urllib.request.Request(
            f"http://{HTTP_HOST}:{HTTP_PORT}/statusline",
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        urllib.request.urlopen(req, timeout=0.3).read()
    except (urllib.error.URLError, OSError, TimeoutError, ValueError):
        pass


def main() -> int:
    try:
        raw = sys.stdin.read()
    except Exception:
        raw = ""

    payload: dict = {}
    if raw.strip():
        try:
            payload = json.loads(raw)
        except (json.JSONDecodeError, ValueError):
            payload = {}

    norm = normalize(payload) if isinstance(payload, dict) else {}
    if norm:
        post_to_daemon(norm)

    sys.stdout.write(format_status_line(norm))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

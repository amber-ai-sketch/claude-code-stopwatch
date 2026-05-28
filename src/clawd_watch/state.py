"""In-memory state for the clawd-watch daemon.

Tracks active Claude Code sessions, pending tool approvals, the latest
statusline payload, and the most recent tool invocation (for the
device's busy.* motion graphic selection). Notifies the heartbeat loop
on every change via a lazy asyncio.Event.
"""
from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Session:
    session_id: str
    running: bool = False
    last_seen: float = field(default_factory=time.time)


@dataclass
class PendingApproval:
    approval_id: str
    session_id: str
    tool: str
    hint: str
    future: "asyncio.Future[str]"  # resolves to "allow" / "deny" / "ask"
    created: float = field(default_factory=time.time)


# Tool name lifetime. The Stop hook clears `current_tool`, but if a Stop
# never fires (CLI killed mid-turn) we still want the device to drop out
# of busy.* eventually.
TOOL_TTL_SECONDS = 60.0


class State:
    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.pending: dict[str, PendingApproval] = {}
        self.approvals_total: int = 0
        self.denials_total: int = 0
        self.started: float = time.time()
        # Latest statusline payload (already normalized to clawd-watch keys).
        self.statusline: dict = {}
        # Most recent PreToolUse tool name + when it was set. Used to drive
        # busy.bash / busy.edit / busy.web motion graphics on the device.
        self.current_tool: Optional[str] = None
        self._tool_set_at: float = 0.0
        # Lazy asyncio.Event — see Python 3.9 loop-binding note below.
        self._changed: Optional[asyncio.Event] = None

    @property
    def changed(self) -> asyncio.Event:
        # Python 3.9 binds asyncio.Event() to the loop current at construction.
        # Defer until the running loop is the real one.
        if self._changed is None:
            self._changed = asyncio.Event()
        return self._changed

    # ─── session updates ────────────────────────────────────────

    def session_seen(self, sid: str) -> Session:
        s = self.sessions.get(sid)
        if not s:
            s = Session(session_id=sid)
            self.sessions[sid] = s
        s.last_seen = time.time()
        self.changed.set()
        return s

    def session_set_running(self, sid: str, running: bool) -> None:
        s = self.session_seen(sid)
        if s.running != running:
            s.running = running
            self.changed.set()

    def session_end(self, sid: str) -> None:
        if self.sessions.pop(sid, None):
            self.changed.set()

    def prune_stale(self, ttl: float = 3600.0) -> None:
        cutoff = time.time() - ttl
        to_drop = [sid for sid, s in self.sessions.items() if s.last_seen < cutoff]
        for sid in to_drop:
            self.sessions.pop(sid, None)
        # Also expire current_tool if it's been sitting too long without a Stop.
        if self.current_tool is not None and (time.time() - self._tool_set_at) > TOOL_TTL_SECONDS:
            self.current_tool = None
            self.changed.set()
        if to_drop:
            self.changed.set()

    # ─── approvals ──────────────────────────────────────────────

    def register_pending(
        self, approval_id: str, session_id: str, tool: str, hint: str
    ) -> PendingApproval:
        loop = asyncio.get_running_loop()
        fut: asyncio.Future[str] = loop.create_future()
        p = PendingApproval(
            approval_id=approval_id,
            session_id=session_id,
            tool=tool,
            hint=hint,
            future=fut,
        )
        self.pending[approval_id] = p
        self.session_seen(session_id)
        self.changed.set()
        return p

    def resolve_pending(self, approval_id: str, decision: str) -> bool:
        p = self.pending.pop(approval_id, None)
        if not p:
            return False
        if not p.future.done():
            p.future.set_result(decision)
        if decision == "allow":
            self.approvals_total += 1
        elif decision == "deny":
            self.denials_total += 1
        self.changed.set()
        return True

    def first_pending(self) -> Optional[PendingApproval]:
        if not self.pending:
            return None
        return min(self.pending.values(), key=lambda p: p.created)

    # ─── statusline + tool tracking ────────────────────────────

    def update_statusline(self, payload: dict) -> None:
        self.statusline = payload
        self.changed.set()

    def set_current_tool(self, tool: Optional[str]) -> None:
        if self.current_tool != tool:
            self.current_tool = tool
            self._tool_set_at = time.time()
            self.changed.set()

    # ─── aggregate view for heartbeats ─────────────────────────

    def heartbeat_snapshot(self) -> dict:
        total = len(self.sessions)
        running = sum(1 for s in self.sessions.values() if s.running)
        waiting = len(self.pending)
        p = self.first_pending()

        if p:
            msg = f"approve: {p.tool}"
            prompt = {"id": p.approval_id, "tool": p.tool, "hint": p.hint}
        elif self.current_tool and running:
            msg = f"running ({self.current_tool})"
            prompt = None
        elif running:
            msg = f"running ({running})" if running > 1 else "running"
            prompt = None
        elif total:
            msg = f"idle ({total})" if total > 1 else "idle"
            prompt = None
        else:
            msg = "no sessions"
            prompt = None

        return {
            "total": total,
            "running": running,
            "waiting": waiting,
            "msg": msg,
            "prompt": prompt,
            # Statusline pass-through. None values get skipped by heartbeat().
            "cost_usd": self.statusline.get("cost_usd"),
            "context_pct": self.statusline.get("context_pct"),
            "rate_5h_pct": self.statusline.get("rate_5h_pct"),
            "rate_7d_pct": self.statusline.get("rate_7d_pct"),
            "model_name": self.statusline.get("model_name"),
            "current_tool": self.current_tool,
        }

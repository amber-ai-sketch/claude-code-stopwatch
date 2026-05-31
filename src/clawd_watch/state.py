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
    # Per-session statusline metrics, set by the statusline hook keyed on sid.
    cost_usd: Optional[float] = None
    context_pct: Optional[float] = None
    model_name: Optional[str] = None
    current_tool: Optional[str] = None
    project: Optional[str] = None          # workspace dir basename, used as title
    input_tokens: Optional[int] = None
    output_tokens: Optional[int] = None
    cache_read_tokens: Optional[int] = None
    cache_create_tokens: Optional[int] = None


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

# Cap how many sessions we describe in a heartbeat. The watch shows one
# detail page per session; beyond a handful it's neither swipeable nor
# MTU-friendly. Extra sessions still count in the total/running tallies.
MAX_DEVICE_SESSIONS = 8


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
        # Route metrics to the owning session so the device's per-session
        # detail pages can show each session's own cost / context / model.
        sid = payload.get("session_id")
        if sid:
            s = self.session_seen(sid)
            if payload.get("cost_usd") is not None:
                s.cost_usd = payload["cost_usd"]
            if payload.get("context_pct") is not None:
                s.context_pct = payload["context_pct"]
            if payload.get("model_name") is not None:
                s.model_name = payload["model_name"]
            if payload.get("project") is not None:
                s.project = payload["project"]
            if payload.get("input_tokens") is not None:
                s.input_tokens = payload["input_tokens"]
            if payload.get("output_tokens") is not None:
                s.output_tokens = payload["output_tokens"]
            if payload.get("cache_read_tokens") is not None:
                s.cache_read_tokens = payload["cache_read_tokens"]
            if payload.get("cache_create_tokens") is not None:
                s.cache_create_tokens = payload["cache_create_tokens"]
        self.changed.set()

    def set_current_tool(self, tool: Optional[str], sid: Optional[str] = None) -> None:
        if sid:
            s = self.sessions.get(sid)
            if s:
                s.current_tool = tool
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

        # Which sessions have a pending approval — drives the per-session
        # "waiting" state (Clawd raises a claw on that detail page).
        waiting_sids = {pa.session_id for pa in self.pending.values()}

        # Per-session detail for the device's swipeable pages. Compact keys
        # and dropped-when-empty fields keep the BLE payload small. Capped at
        # MAX_DEVICE_SESSIONS — the watch can't usefully page beyond that and
        # an unbounded array would blow past a sane heartbeat size. Stable
        # order = creation order so pages don't reshuffle under the user.
        session_details = []
        for s in list(self.sessions.values())[:MAX_DEVICE_SESSIONS]:
            d: dict = {
                "sid": s.session_id[:8],
                "run": 1 if s.running else 0,
                "wait": 1 if s.session_id in waiting_sids else 0,
            }
            if s.context_pct is not None:
                d["ctx"] = round(s.context_pct, 1)
            if s.cost_usd is not None:
                d["cost"] = round(s.cost_usd, 2)
            if s.model_name:
                d["model"] = s.model_name
            if s.current_tool:
                d["tool"] = s.current_tool
            if s.project:
                d["proj"] = s.project
            if s.input_tokens is not None:
                d["tin"] = s.input_tokens
            if s.output_tokens is not None:
                d["tout"] = s.output_tokens
            if s.cache_read_tokens is not None:
                d["cr"] = s.cache_read_tokens
            if s.cache_create_tokens is not None:
                d["cw"] = s.cache_create_tokens
            session_details.append(d)

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
            "sessions": session_details,
        }

"""Tests for clawd_watch.state — session management, approvals, heartbeat snapshot."""
from __future__ import annotations

import asyncio
import time

import pytest

from clawd_watch.state import State, Session, PendingApproval, TOOL_TTL_SECONDS, MAX_DEVICE_SESSIONS


# ─── session lifecycle ──────────────────────────────────────────

def test_session_seen_creates_new():
    state = State()
    s = state.session_seen("s1")
    assert s.session_id == "s1"
    assert "s1" in state.sessions


def test_session_seen_updates_existing():
    state = State()
    s1 = state.session_seen("s1")
    t1 = s1.last_seen
    time.sleep(0.01)
    s2 = state.session_seen("s1")
    assert s2 is s1
    assert s2.last_seen > t1


def test_session_set_running():
    state = State()
    state.session_set_running("s1", True)
    assert state.sessions["s1"].running is True
    state.session_set_running("s1", False)
    assert state.sessions["s1"].running is False


def test_session_end_removes():
    state = State()
    state.session_seen("s1")
    state.session_end("s1")
    assert "s1" not in state.sessions


def test_session_end_nonexistent():
    state = State()
    state.session_end("ghost")  # should not raise


def test_prune_stale_removes_old_sessions():
    state = State()
    s = state.session_seen("old")
    s.last_seen = time.time() - 7200  # 2 hours ago
    state.session_seen("new")
    state.prune_stale(ttl=3600)
    assert "old" not in state.sessions
    assert "new" in state.sessions


def test_prune_stale_expires_current_tool():
    state = State()
    state.set_current_tool("Bash")
    state._tool_set_at = time.time() - TOOL_TTL_SECONDS - 1
    state.prune_stale()
    assert state.current_tool is None


# ─── approvals ─────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_register_and_resolve_pending():
    state = State()
    p = state.register_pending("a1", "s1", "Bash", "echo hi")
    assert p.approval_id == "a1"
    assert "a1" in state.pending

    resolved = state.resolve_pending("a1", "allow")
    assert resolved is True
    assert "a1" not in state.pending
    assert state.approvals_total == 1
    assert await p.future == "allow"


@pytest.mark.asyncio
async def test_resolve_deny_increments_denials():
    state = State()
    p = state.register_pending("a2", "s1", "Bash", "rm -rf")
    state.resolve_pending("a2", "deny")
    assert state.denials_total == 1
    assert await p.future == "deny"


def test_resolve_nonexistent_returns_false():
    state = State()
    assert state.resolve_pending("ghost", "allow") is False


@pytest.mark.asyncio
async def test_first_pending_returns_oldest():
    state = State()
    p1 = state.register_pending("a1", "s1", "Bash", "first")
    time.sleep(0.01)
    p2 = state.register_pending("a2", "s1", "Bash", "second")
    assert state.first_pending() is p1


def test_first_pending_empty():
    state = State()
    assert state.first_pending() is None


# ─── statusline ─────────────────────────────────────────────────

def test_update_statusline_routes_to_session():
    state = State()
    state.update_statusline({
        "session_id": "s1",
        "cost_usd": 1.23,
        "context_pct": 45.6,
        "model_name": "claude-sonnet-4-6",
    })
    s = state.sessions["s1"]
    assert s.cost_usd == 1.23
    assert s.context_pct == 45.6
    assert s.model_name == "claude-sonnet-4-6"


def test_update_statusline_without_session_id():
    state = State()
    state.update_statusline({"cost_usd": 0.5})  # no session_id
    assert state.statusline == {"cost_usd": 0.5}


# ─── set_current_tool ──────────────────────────────────────────

def test_set_current_tool_tracks_per_session():
    state = State()
    state.session_seen("s1")
    state.set_current_tool("Bash", sid="s1")
    assert state.sessions["s1"].current_tool == "Bash"
    assert state.current_tool == "Bash"


# ─── heartbeat_snapshot ────────────────────────────────────────

def test_heartbeat_snapshot_empty():
    state = State()
    snap = state.heartbeat_snapshot()
    assert snap["total"] == 0
    assert snap["running"] == 0
    assert snap["waiting"] == 0
    assert snap["msg"] == "no sessions"


def test_heartbeat_snapshot_running():
    state = State()
    state.session_set_running("s1", True)
    snap = state.heartbeat_snapshot()
    assert snap["total"] == 1
    assert snap["running"] == 1
    assert "running" in snap["msg"]


def test_heartbeat_snapshot_idle():
    state = State()
    state.session_seen("s1")
    snap = state.heartbeat_snapshot()
    assert snap["total"] == 1
    assert snap["running"] == 0
    assert "idle" in snap["msg"]


async def test_heartbeat_snapshot_with_pending():
    state = State()
    state.register_pending("a1", "s1", "Bash", "echo hi")
    snap = state.heartbeat_snapshot()
    assert snap["waiting"] == 1
    assert "approve" in snap["msg"]
    assert snap["prompt"]["id"] == "a1"


def test_heartbeat_snapshot_caps_sessions():
    state = State()
    for i in range(MAX_DEVICE_SESSIONS + 5):
        state.session_seen(f"s{i}")
    snap = state.heartbeat_snapshot()
    assert len(snap["sessions"]) == MAX_DEVICE_SESSIONS
    assert snap["total"] == MAX_DEVICE_SESSIONS + 5  # uncapped in total


def test_heartbeat_snapshot_session_detail_fields():
    state = State()
    state.update_statusline({
        "session_id": "s1",
        "cost_usd": 2.50,
        "context_pct": 80.0,
        "model_name": "claude-sonnet-4-6",
        "project": "my-project",
        "input_tokens": 1000,
        "output_tokens": 500,
    })
    snap = state.heartbeat_snapshot()
    detail = snap["sessions"][0]
    assert detail["sid"] == "s1"  # truncated to 8 chars
    assert detail["cost"] == 2.50
    assert detail["ctx"] == 80.0
    assert detail["model"] == "claude-sonnet-4-6"
    assert detail["title"] == "my-project"
    assert detail["tin"] == 1000
    assert detail["tout"] == 500

"""Tests for clawd_watch.protocol — BLE wire protocol message construction."""
from __future__ import annotations

import pytest

from clawd_watch.protocol import (
    heartbeat,
    status_ack,
    generic_ack,
    short_hint_for_tool,
    time_sync_msg,
    owner_msg,
    NUS_SERVICE,
    NUS_RX,
    NUS_TX,
    AUDIO_SERVICE,
    AUDIO_TX,
)


# ─── constants ──────────────────────────────────────────────────

def test_nus_uuids_are_valid():
    assert len(NUS_SERVICE) == 36
    assert len(NUS_RX) == 36
    assert len(NUS_TX) == 36
    assert NUS_SERVICE != NUS_RX != NUS_TX


def test_audio_uuids_are_valid():
    assert len(AUDIO_SERVICE) == 36
    assert len(AUDIO_TX) == 36
    assert AUDIO_SERVICE != AUDIO_TX


# ─── heartbeat ─────────────────────────────────────────────────

def test_heartbeat_minimal():
    h = heartbeat(total=1, running=0, waiting=0, msg="idle")
    assert h == {"total": 1, "running": 0, "waiting": 0, "msg": "idle"}


def test_heartbeat_with_statusline():
    h = heartbeat(
        total=2, running=1, waiting=0, msg="running",
        cost_usd=1.234, context_pct=55.678, model_name="claude-sonnet-4-6",
        rate_5h_pct=10.0, rate_7d_pct=5.0,
    )
    assert h["cost"] == 1.23  # rounded to 2 decimals
    assert h["ctx"] == 55.7   # rounded to 1 decimal
    assert h["model"] == "claude-sonnet-4-6"
    assert h["r5h"] == 10.0
    assert h["r7d"] == 5.0


def test_heartbeat_with_prompt():
    prompt = {"id": "abc", "tool": "Bash", "hint": "echo hi"}
    h = heartbeat(total=1, running=1, waiting=1, msg="approve", prompt=prompt)
    assert h["prompt"] == prompt


def test_heartbeat_none_fields_excluded():
    h = heartbeat(total=0, running=0, waiting=0, msg="no sessions")
    assert "cost" not in h
    assert "ctx" not in h
    assert "model" not in h
    assert "tool" not in h
    assert "prompt" not in h
    assert "sessions" not in h


def test_heartbeat_with_sessions():
    sessions = [{"sid": "s1", "run": 1}]
    h = heartbeat(total=1, running=1, waiting=0, msg="running", sessions=sessions)
    assert h["sessions"] == sessions


def test_heartbeat_with_current_tool():
    h = heartbeat(total=1, running=1, waiting=0, msg="running", current_tool="Bash")
    assert h["tool"] == "Bash"


# ─── status_ack ────────────────────────────────────────────────

def test_status_ack_default():
    ack = status_ack()
    assert ack["ack"] == "status"
    assert ack["ok"] is True
    assert ack["data"]["name"] == "clawd-watch"
    assert ack["data"]["sec"] is False


def test_status_ack_with_values():
    ack = status_ack(name="test", secure=True, approvals=5, denials=2, up_seconds=3600)
    assert ack["data"]["name"] == "test"
    assert ack["data"]["sec"] is True
    assert ack["data"]["stats"]["appr"] == 5
    assert ack["data"]["stats"]["deny"] == 2
    assert ack["data"]["sys"]["up"] == 3600


# ─── generic_ack ───────────────────────────────────────────────

def test_generic_ack_default():
    ack = generic_ack("ping")
    assert ack == {"ack": "ping", "ok": True, "n": 0}


def test_generic_ack_not_ok():
    ack = generic_ack("fail", ok=False)
    assert ack["ok"] is False


# ─── short_hint_for_tool ──────────────────────────────────────

def test_hint_bash():
    assert short_hint_for_tool("Bash", {"command": "echo hello"}) == "echo hello"


def test_hint_bash_multiline():
    hint = short_hint_for_tool("Bash", {"command": "line1\nline2\nline3"})
    assert hint == "line1"  # first line only


def test_hint_bash_truncates():
    long_cmd = "x" * 200
    hint = short_hint_for_tool("Bash", {"command": long_cmd})
    assert len(hint) == 120


def test_hint_write():
    assert short_hint_for_tool("Write", {"file_path": "/tmp/test.py"}) == "/tmp/test.py"


def test_hint_edit():
    assert short_hint_for_tool("Edit", {"file_path": "/tmp/test.py"}) == "/tmp/test.py"


def test_hint_webfetch():
    assert short_hint_for_tool("WebFetch", {"url": "https://example.com"}) == "https://example.com"


def test_hint_websearch():
    assert short_hint_for_tool("WebSearch", {"query": "python testing"}) == "python testing"


def test_hint_unknown_tool():
    hint = short_hint_for_tool("UnknownTool", {"foo": "bar"})
    assert hint == "bar"


def test_hint_empty_input():
    assert short_hint_for_tool("Bash", {}) == ""


def test_hint_non_dict_input():
    assert short_hint_for_tool("Bash", "not a dict") == ""


# ─── time_sync_msg / owner_msg ─────────────────────────────────

def test_time_sync_msg_structure():
    msg = time_sync_msg()
    assert "time" in msg
    assert len(msg["time"]) == 2
    assert isinstance(msg["time"][0], int)


def test_owner_msg():
    msg = owner_msg("alice")
    assert msg == {"cmd": "owner", "name": "alice"}

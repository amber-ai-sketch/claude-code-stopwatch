"""Shared HTTP helpers for talking to the daemon's localhost API.

Used by hook.py, cli.py, menubar.py. Keeps the request boilerplate in one
place so changes to URL construction, headers, or error handling propagate
to all callers.
"""
from __future__ import annotations

import json
import urllib.request

from . import HTTP_HOST, HTTP_PORT


def daemon_get(path: str, timeout: float = 5.0) -> dict:
    """GET request to the daemon, return parsed JSON response."""
    req = urllib.request.Request(f"http://{HTTP_HOST}:{HTTP_PORT}{path}")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def daemon_post(
    path: str, payload: dict | None = None, timeout: float = 5.0
) -> dict:
    """POST JSON to the daemon, return parsed JSON response."""
    data = json.dumps(payload or {}).encode()
    req = urllib.request.Request(
        f"http://{HTTP_HOST}:{HTTP_PORT}{path}",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())

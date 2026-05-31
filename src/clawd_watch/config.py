"""Persisted clawd-watch config — currently just the last-connected BLE address.

Stored next to the daemon log at ~/.claude/clawd-watch-config.json. The daemon
saves the address on every successful connect and reads it on startup so a Mac
reboot reconnects directly instead of blind-scanning by name.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

from . import CONFIG_PATH


def load_address(path: str = CONFIG_PATH) -> Optional[str]:
    """Return the persisted BLE address, or None if there's no config yet.

    A config file that exists but doesn't parse is a real error (corrupt file
    or wrong shape), not a "no device" state — raise so the daemon log shows it
    instead of silently falling back to scanning.
    """
    p = Path(path)
    if not p.exists():
        return None
    data = json.loads(p.read_text())  # JSONDecodeError propagates — fail fast
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object, got {type(data).__name__}")
    address = data.get("address")
    if address is not None and not isinstance(address, str):
        raise ValueError(f"{path}: 'address' must be a string, got {address!r}")
    return address or None


def save_address(address: str, path: str = CONFIG_PATH) -> None:
    if not address or not isinstance(address, str):
        raise ValueError(f"refusing to save bad address: {address!r}")
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"address": address}, indent=2))


def clear_address(path: str = CONFIG_PATH) -> None:
    Path(path).unlink(missing_ok=True)

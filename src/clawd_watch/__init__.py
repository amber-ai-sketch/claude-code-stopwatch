"""clawd-watch — Claude Code companion for the M5Stack Stopwatch."""
import os

__version__ = "0.1.0"

HTTP_HOST = "127.0.0.1"
HTTP_PORT = 9877  # one above buddy-bridge's 9876 to coexist
LOG_PATH = os.path.expanduser("~/.claude/clawd-watch.log")
# Persisted BLE address of the last-connected watch. Lets the daemon connect
# directly on the next boot instead of blind-scanning (which times out after
# a macOS reboot clears CoreBluetooth's device cache).
CONFIG_PATH = os.path.expanduser("~/.claude/clawd-watch-config.json")

# BLE device advertising name prefix the daemon scans for.
DEVICE_NAME_PREFIX = "ClawdWatch"

# Timeout hierarchy (must stay in sync with settings.json hook timeout = 35s):
#   HOOK_SETTINGS_TIMEOUT  = 35  — Claude Code's hook timeout in settings.json
#   HOOK_REQUEST_TIMEOUT   = 32  — hook.py HTTP call to daemon (slightly under)
#   DAEMON_APPROVAL_TIMEOUT = 30 — daemon waits for watch button (slightly under hook)
HOOK_SETTINGS_TIMEOUT = 35.0
HOOK_REQUEST_TIMEOUT = 32.0
DAEMON_APPROVAL_TIMEOUT = 30.0
HEARTBEAT_INTERVAL = 10.0

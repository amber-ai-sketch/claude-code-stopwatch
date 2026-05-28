"""clawd-watch — Claude Code companion for the M5Stack Stopwatch."""
import os

__version__ = "0.1.0"

HTTP_HOST = "127.0.0.1"
HTTP_PORT = 9877  # one above buddy-bridge's 9876 to coexist
LOG_PATH = os.path.expanduser("~/.claude/clawd-watch.log")

# BLE device advertising name prefix the daemon scans for.
DEVICE_NAME_PREFIX = "ClawdWatch"

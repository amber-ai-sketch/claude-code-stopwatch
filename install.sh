#!/bin/bash
# Install clawd-watch as a launchd user agent and wire up the Claude Code
# hooks + statusLine in ~/.claude/settings.json.
#
# Idempotent: safe to run multiple times.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="$ROOT/.venv"
PY="${PY:-python3}"
LOCAL_BIN="$HOME/.local/bin"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
LABEL="com.claude-code.clawd-watch"
PLIST_SRC="$ROOT/resources/${LABEL}.plist"
PLIST_DST="$LAUNCH_AGENTS/${LABEL}.plist"
LOG_PATH="$HOME/.claude/clawd-watch.log"
PYPI_MIRROR="${PYPI_MIRROR:-https://pypi.tuna.tsinghua.edu.cn/simple}"

say() { printf "\033[1;36m==>\033[0m %s\n" "$*"; }

# 1. venv -------------------------------------------------------------------
if [[ ! -x "$VENV/bin/python" ]]; then
  say "Creating venv at $VENV"
  "$PY" -m venv "$VENV"
fi

say "Installing clawd-watch (editable) from $PYPI_MIRROR"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -i "$PYPI_MIRROR" -e "$ROOT"

# 2. link CLI entrypoints ---------------------------------------------------
mkdir -p "$LOCAL_BIN"
for bin in clawd-watch-daemon clawd-watch-hook clawd-watch clawd-statusline; do
  src="$VENV/bin/$bin"
  dst="$LOCAL_BIN/$bin"
  if [[ ! -x "$src" ]]; then
    echo "error: $src not found (pip install failed?)" >&2
    exit 1
  fi
  ln -sfn "$src" "$dst"
done
say "Linked entrypoints into $LOCAL_BIN"

case ":$PATH:" in
  *":$LOCAL_BIN:"*) ;;
  *) say "Note: $LOCAL_BIN is not on \$PATH — add it for 'clawd-watch' to work from the shell" ;;
esac

# 3. launchd agent ----------------------------------------------------------
mkdir -p "$LAUNCH_AGENTS" "$(dirname "$LOG_PATH")"
sed -e "s|__PROJECT_ROOT__|$ROOT|g" \
    -e "s|__LOG_PATH__|$LOG_PATH|g" \
    "$PLIST_SRC" > "$PLIST_DST"

if launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1; then
  say "Unloading previous $LABEL"
  launchctl bootout "gui/$UID/$LABEL" || true
fi

say "Loading $LABEL into launchd"
launchctl bootstrap "gui/$UID" "$PLIST_DST"
launchctl enable "gui/$UID/$LABEL" || true

# 4. patch ~/.claude/settings.json ------------------------------------------
SETTINGS="$HOME/.claude/settings.json"
if [[ -f "$SETTINGS" ]]; then
  BACKUP="$SETTINGS.bak.$(date +%Y%m%d-%H%M%S)"
  cp -p "$SETTINGS" "$BACKUP"
  say "Backed up existing settings.json to $BACKUP"
fi
say "Merging hooks + statusLine into $SETTINGS"
"$VENV/bin/clawd-watch" install-hooks \
  --hook-bin "$LOCAL_BIN/clawd-watch-hook" \
  --statusline-bin "$LOCAL_BIN/clawd-statusline"

cat <<EOF

✓ Install complete.

Quick sanity check (no firmware needed yet):
  clawd-watch test-statusline    # push a fake payload to the daemon
  clawd-watch status             # see it land in 'line:' summary

Once the firmware is flashed and powered on:
  1. Wake the watch (any button).
  2. macOS may ask for Bluetooth permission for clawd-watch-daemon.
     System Settings → Privacy & Security → Bluetooth → grant it.
  3. clawd-watch status     # 'ble: ✓ connected'
  4. clawd-watch test       # injects a fake approval — left=allow, right=deny

Logs: $LOG_PATH
Tail: clawd-watch tail
EOF

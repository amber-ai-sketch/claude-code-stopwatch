#!/bin/bash
# Tear down everything install.sh did. Leaves the venv and source tree alone.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="$ROOT/.venv"
LOCAL_BIN="$HOME/.local/bin"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
LABEL="com.claude-code.clawd-watch"
PLIST_DST="$LAUNCH_AGENTS/${LABEL}.plist"
UI_LABEL="com.claude-code.clawd-watch-ui"
UI_PLIST_DST="$LAUNCH_AGENTS/${UI_LABEL}.plist"

say() { printf "\033[1;36m==>\033[0m %s\n" "$*"; }

for label in "$UI_LABEL" "$LABEL"; do
  if launchctl print "gui/$UID/$label" >/dev/null 2>&1; then
    say "Unloading $label"
    launchctl bootout "gui/$UID/$label" || true
  fi
done
rm -f "$PLIST_DST" "$UI_PLIST_DST"

for bin in clawd-watch-daemon clawd-watch-hook clawd-watch clawd-watch-ui clawd-statusline; do
  rm -f "$LOCAL_BIN/$bin"
done
say "Removed CLI entrypoints from $LOCAL_BIN"

if [[ -x "$VENV/bin/clawd-watch" ]]; then
  "$VENV/bin/clawd-watch" uninstall-hooks || true
fi

say "Done. venv at $VENV left intact."

#!/bin/bash
# Tear down everything install.sh did. Leaves the venv and source tree alone.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="$ROOT/.venv"
LOCAL_BIN="$HOME/.local/bin"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
LABEL="com.claude-code.clawd-watch"
PLIST_DST="$LAUNCH_AGENTS/${LABEL}.plist"

say() { printf "\033[1;36m==>\033[0m %s\n" "$*"; }

if launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1; then
  say "Unloading $LABEL"
  launchctl bootout "gui/$UID/$LABEL" || true
fi
rm -f "$PLIST_DST"

for bin in clawd-watch-daemon clawd-watch-hook clawd-watch clawd-statusline; do
  rm -f "$LOCAL_BIN/$bin"
done
say "Removed CLI entrypoints from $LOCAL_BIN"

if [[ -x "$VENV/bin/clawd-watch" ]]; then
  "$VENV/bin/clawd-watch" uninstall-hooks || true
fi

say "Done. venv at $VENV left intact."

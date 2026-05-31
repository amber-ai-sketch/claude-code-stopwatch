#!/bin/bash
# Apply our additions to a fresh clone of m5stack/M5StopWatch-UserDemo.
#
# Usage:
#   git clone --depth 1 https://github.com/m5stack/M5StopWatch-UserDemo.git upstream
#   cd ..
#   ./apply_to_upstream.sh
#
# Idempotent: re-running on an already-patched clone applies cleanly because
# we use `git apply -3 --check` first, and the file copy uses cp which
# overwrites in place.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
UPSTREAM="$ROOT/upstream"

if [[ ! -d "$UPSTREAM/main" ]]; then
  echo "error: $UPSTREAM doesn't look like an M5StopWatch-UserDemo clone." >&2
  echo "Run from firmware/ after cloning M5StopWatch-UserDemo into 'upstream/'." >&2
  exit 1
fi

# 1. Apply upstream-edit patches (main.cpp + apps.h tweaks).
for p in "$ROOT"/patches/*.patch; do
  echo "==> applying $(basename "$p")"
  if git -C "$UPSTREAM" apply --check "$p" 2>/dev/null; then
    git -C "$UPSTREAM" apply "$p"
  elif git -C "$UPSTREAM" apply --check --reverse "$p" 2>/dev/null; then
    echo "    (already applied; skipping)"
  else
    echo "error: patch $p does not apply cleanly. Upstream may have moved." >&2
    exit 1
  fi
done

# 2. Drop our app_claude/ tree into main/apps/.
TARGET="$UPSTREAM/main/apps/app_claude"
mkdir -p "$TARGET"
echo "==> copying app_claude/ into $TARGET"
cp -R "$ROOT/app_claude/." "$TARGET/"

# 3. Touch CMakeLists so GLOB_RECURSE re-runs and picks up newly added source
# files. Without this, adding a new .cpp under app_claude/ leaves it out of the
# build (stale glob) and you get undefined-reference link errors.
touch "$UPSTREAM/main/CMakeLists.txt"
echo "==> touched main/CMakeLists.txt (refresh GLOB_RECURSE)"

echo "✓ upstream patched."
echo "Next: cd upstream && python3 fetch_repos.py && idf.py set-target esp32s3 && idf.py build"

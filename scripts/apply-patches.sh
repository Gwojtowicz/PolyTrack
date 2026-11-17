#!/usr/bin/env bash
# Applies PolyTrack's iPlug2 changes (patches/iPlug2.patch) to the pinned iPlug2 submodule.
# Safe to run more than once: an already applied patch is skipped.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
git -C "$root" submodule update --init --recursive
patch="$root/patches/iPlug2.patch"
if git -C "$root/iPlug2" apply --reverse --check --whitespace=nowarn "$patch" 2>/dev/null; then
  echo "iPlug2: already applied"; exit 0
fi
git -C "$root/iPlug2" apply --whitespace=nowarn "$patch" 2>/dev/null
echo "iPlug2: applied"

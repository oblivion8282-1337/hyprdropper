#!/usr/bin/env bash
# Called by the pacman hook (as the user) after hyprland was installed/upgraded.
# Rebuilds against the new headers. Never approves a new minor version by itself:
# if the minor changed, the plugin stays inactive (it shows a notification) until
# scripts/approve.sh has been run.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGDIR="${HOME}/.cache/hyprdropper"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/rebuild.log"

{
    echo "=== $(date -Is) rebuild for Hyprland $(pkg-config --modversion hyprland 2>/dev/null)"
    if "$ROOT/scripts/install.sh"; then
        echo "ok"
    else
        echo "FAILED - the old plugin build stays in place; Hyprland will refuse to load it (version mismatch) and keeps working without it."
    fi
} >>"$LOG" 2>&1

tail -n 3 "$LOG"
exit 0 # never fail the pacman transaction

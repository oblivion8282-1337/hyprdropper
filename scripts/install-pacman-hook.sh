#!/usr/bin/env bash
# Arch/CachyOS, manual install only (not needed with hyprpm): install a pacman hook that
# rebuilds the plugin for the current user whenever the hyprland package is upgraded.
#   scripts/install-pacman-hook.sh            install (asks for sudo)
#   scripts/install-pacman-hook.sh --remove   remove it again
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOK=/etc/pacman.d/hooks/90-hyprdropper.hook

if [[ "${1:-}" == "--remove" ]]; then
    sudo rm -f "$HOOK"
    echo "removed $HOOK"
    exit 0
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
sed -e "s|@USER@|$(id -un)|" \
    -e "s|@HOME@|$HOME|" \
    -e "s|@XDG_CONFIG_HOME@|${XDG_CONFIG_HOME:-$HOME/.config}|" \
    -e "s|@REPO@|$ROOT|" \
    "$ROOT/packaging/90-hyprdropper.hook.in" >"$TMP"

sudo install -Dm0644 "$TMP" "$HOOK"
echo "installed $HOOK:"
cat "$HOOK"

#!/usr/bin/env bash
# Build, unit-test and install the plugin for the currently installed Hyprland headers.
#   scripts/install.sh            build + install
#   scripts/install.sh --approve  additionally approve the current Hyprland minor version
#                                 (only do this after scripts/approve.sh passed or by hand on purpose)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${HOME}/.local/lib/hyprdropper"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/hyprdropper"

cd "$ROOT"
make -s clean
make -s
make -s test >/dev/null

mkdir -p "$DEST"
# atomic replace: a running Hyprland may have the old file mapped
install -m 0755 build/hyprdropper.so "$DEST/.hyprdropper.so.new"
mv -f "$DEST/.hyprdropper.so.new" "$DEST/hyprdropper.so"

HYPR_VER="$(pkg-config --modversion hyprland)"
echo "installed $DEST/hyprdropper.so (built against Hyprland headers $HYPR_VER)"

if [[ "${1:-}" == "--approve" ]]; then
    MINOR="$(echo "$HYPR_VER" | cut -d. -f1,2)"
    mkdir -p "$CONF"
    touch "$CONF/approved-versions"
    if ! grep -qx "$MINOR" "$CONF/approved-versions"; then
        echo "$MINOR" >>"$CONF/approved-versions"
    fi
    echo "approved Hyprland $MINOR in $CONF/approved-versions"
fi

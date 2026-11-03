#!/usr/bin/env bash
# Approve the installed Hyprland minor version after the full test suite passed in a
# throw-away nested Hyprland (your session is not touched).
#
#   scripts/approve.sh
#
# Needs: git, mutter (headless parent), alacritty, python3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HYPR_VER="$(pkg-config --modversion hyprland)"
RUNNING="$(Hyprland --version 2>/dev/null | sed -n 's/.*Tag: v\([0-9.]*\).*/\1/p' | head -1)"
MINOR="$(echo "$HYPR_VER" | cut -d. -f1,2)"

if [[ -n "$RUNNING" && "$RUNNING" != "$HYPR_VER" ]]; then
    echo "note: headers are $HYPR_VER, Hyprland binary reports $RUNNING"
fi

echo "== building plugin for Hyprland $HYPR_VER"
cd "$ROOT"
make -s clean && make -s && make -s test

echo "== building Hyprland's test plugin (v$HYPR_VER)"
"$ROOT/scripts/build-tester.sh" "$HYPR_VER"
TESTER="${HOME}/.cache/hyprdropper/tester-$HYPR_VER/hyprtestplugin.so"

echo "== running integration tests in a nested Hyprland"
python3 "$ROOT/tests/headless/run_tests.py" --plugin "$ROOT/build/hyprdropper.so" --tester "$TESTER"

echo
read -r -p "All tests passed. Approve Hyprland $MINOR and install? [y/N] " ans
if [[ "$ans" == "y" || "$ans" == "Y" ]]; then
    "$ROOT/scripts/install.sh" --approve
    echo "Done. Reload the plugin: hyprctl plugin unload ~/.local/lib/hyprdropper/hyprdropper.so; hyprctl reload"
else
    echo "Not approved."
fi

#!/usr/bin/env bash
# Build Hyprland's own hyprtester plugin (input simulation for the integration tests)
# for the given Hyprland version, against the installed headers.
#   scripts/build-tester.sh 0.57.0
set -euo pipefail

VER="${1:-$(pkg-config --modversion hyprland)}"
OUT="${HOME}/.cache/hyprdropper/tester-$VER"
SO="$OUT/hyprtestplugin.so"

if [[ -f "$SO" ]]; then
    echo "tester plugin already built: $SO"
    exit 0
fi

mkdir -p "$OUT"
if [[ ! -d "$OUT/src" ]]; then
    git clone -q --depth 1 --branch "v$VER" https://github.com/hyprwm/Hyprland.git "$OUT/src"
fi

cd "$OUT/src/hyprtester/plugin"
g++ -shared -fPIC -O1 -std=c++2b -Wno-c++11-narrowing \
    src/main.cpp src/WorkspaceGestures.cpp src/SpecialWorkspaceGestures.cpp src/XWaylandSelection.cpp \
    -o "$SO" \
    $(pkg-config --cflags hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon lua) \
    $(pkg-config --libs pangocairo)

echo "built $SO"

#!/usr/bin/env bash
# build-foot.sh — foot terminal for LunaOS arm64
# arm64: -arch arm64, Homebrew at /opt/homebrew, NEON optimized

set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${LUNA_ROOT}/build/rootfs/usr/local"
SRC="${LUNA_ROOT}/build/apps-src/foot"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${BREW_PREFIX}/lib/pkgconfig"
export CFLAGS="-arch arm64 -mcpu=apple-m1"

log() { echo -e "\033[0;32m[foot-arm64]\033[0m $*"; }
log "Building foot terminal (arm64)"

if [[ ! -d "$SRC/.git" ]]; then
    git clone --depth 1 --branch 1.19.0 \
        https://codeberg.org/dnkl/foot.git "$SRC" --quiet
fi

meson setup "${LUNA_ROOT}/build/obj/foot" "$SRC" \
    --prefix="$PREFIX" --buildtype=release \
    -Dterminfo=disabled \
    -Dfcft:text-shaping=disabled \
    -Dc_args="-arch arm64 -mcpu=apple-m1" \
    -Dc_link_args="-arch arm64" 2>&1 | tail -5

ninja -C "${LUNA_ROOT}/build/obj/foot" -j"$JOBS"
ninja -C "${LUNA_ROOT}/build/obj/foot" install

mkdir -p "${LUNA_ROOT}/build/rootfs/etc/foot"
cat > "${LUNA_ROOT}/build/rootfs/etc/foot/foot.ini" <<'INI'
[main]
font=monospace:size=13
pad=6x6
term=foot

[colors]
background=0d1117
foreground=e6edf3
regular0=21262d
regular1=ff7b72
regular2=3fb950
regular3=d29922
regular4=58a6ff
regular5=bc8cff
regular6=39d353
regular7=b1bac4

[key-bindings]
scrollback-up-page=Shift+Page_Up
scrollback-down-page=Shift+Page_Down
clipboard-copy=Control+Shift+c
clipboard-paste=Control+Shift+v
INI

mkdir -p "${PREFIX}/share/applications"
cat > "${PREFIX}/share/applications/foot.desktop" <<'DESKTOP'
[Desktop Entry]
Name=Terminal
Exec=foot
Icon=terminal
Type=Application
Categories=System;TerminalEmulator;
DESKTOP

log "foot installed → ${PREFIX}/bin/foot"

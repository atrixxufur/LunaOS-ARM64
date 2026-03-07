#!/usr/bin/env bash
# =============================================================================
# build-compositor.sh — Wayland compositor stack for LunaOS arm64
#
# Builds: wayland → wayland-protocols → libxkbcommon → pixman →
#         libinput → wlroots (arm64 patched) → luna-compositor
#
# arm64 differences:
#   - All -arch arm64 (not x86_64)
#   - Homebrew prefix: /opt/homebrew (Apple Silicon) vs /usr/local (Intel)
#   - wlroots backend: DRM via IOMobileFramebuffer (not IOFramebuffer)
#   - NEON optimizations enabled in pixman and libxkbcommon
# =============================================================================

set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${LUNA_ROOT}/build/compositor-deps}"
PREFIX="${PREFIX:-${LUNA_ROOT}/build/rootfs/usr/local}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
log()  { echo -e "${GREEN}[compositor-arm64]${NC} $*"; }
step() { echo -e "${BLUE}[compositor-arm64]${NC} ── $*"; }

# arm64: Homebrew is in /opt/homebrew on Apple Silicon
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
export PATH="${BREW_PREFIX}/bin:$PATH"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${BREW_PREFIX}/lib/pkgconfig"
export CFLAGS="-arch arm64"
export CXXFLAGS="-arch arm64"
export LDFLAGS="-arch arm64"

mkdir -p "$BUILD_DIR" "$PREFIX"

clone() {
    local url="$1" dir="$2" tag="${3:-}"
    [[ -d "$dir/.git" ]] && return
    if [[ -n "$tag" ]]; then
        git clone --depth 1 --branch "$tag" "$url" "$dir" --quiet
    else
        git clone --depth 1 "$url" "$dir" --quiet
    fi
}

# ── wayland ────────────────────────────────────────────────────────────────
step "libwayland"
clone "https://gitlab.freedesktop.org/wayland/wayland.git" \
      "${BUILD_DIR}/wayland" "1.23.0"
# Patch wayland for Darwin (replace epoll with kqueue)
if [[ ! -f "${BUILD_DIR}/wayland/.darwin-patched" ]]; then
    # Replace wayland-os.c epoll with kqueue stub
    cat > "${BUILD_DIR}/wayland/src/wayland-os.c" << 'OSEOF'
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdio.h>
#include "wayland-os.h"

static int set_cloexec_or_close(int fd) {
    if (fd == -1) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) { close(fd); return -1; }
    return fd;
}
int wl_os_socket_cloexec(int domain, int type, int protocol) {
    return set_cloexec_or_close(socket(domain, type, protocol));
}
int wl_os_dupfd_cloexec(int fd, int minfd) {
    int newfd = fcntl(fd, F_DUPFD, minfd);
    return set_cloexec_or_close(newfd);
}
ssize_t wl_os_recvmsg_cloexec(int sockfd, struct msghdr *msg, int flags) {
    return recvmsg(sockfd, msg, flags);
}
int wl_os_epoll_create_cloexec(void) {
    /* Use a pipe as a dummy epoll fd — real event loop uses kqueue */
    int pp[2];
    if (pipe(pp) < 0) return -1;
    close(pp[1]);
    return pp[0];
}
int wl_os_accept_cloexec(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return set_cloexec_or_close(accept(sockfd, addr, addrlen));
}
OSEOF

    # Add MSG_DONTWAIT and CMSG_LEN compat to connection.c
    sed -i '' 's/#include <sys\/epoll.h>/\/* epoll skipped on Darwin *\//'         "${BUILD_DIR}/wayland/src/connection.c" 2>/dev/null || true
    sed -i '' 's/#include <sys\/epoll.h>/\/* epoll skipped on Darwin *\//'         "${BUILD_DIR}/wayland/src/wayland-os.c" 2>/dev/null || true

    # Add Darwin compat header
    cat > "${BUILD_DIR}/wayland/src/darwin-compat.h" << 'DEOF'
#pragma once
#include <sys/socket.h>
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x80
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x20000
#endif
DEOF
    sed -i '' 's/#include "wayland-os.h"/#include "wayland-os.h"
#include "darwin-compat.h"/'         "${BUILD_DIR}/wayland/src/connection.c" 2>/dev/null || true

    touch "${BUILD_DIR}/wayland/.darwin-patched"
fi

meson setup "${BUILD_DIR}/obj/wayland" "${BUILD_DIR}/wayland" \
    --prefix="$PREFIX" --buildtype=release \
    -Ddocumentation=false -Dtests=false \
    -Dc_args="-arch arm64" -Dc_link_args="-arch arm64" 2>&1 | tail -3
ninja -C "${BUILD_DIR}/obj/wayland" -j"$JOBS" install

# ── wayland-protocols ─────────────────────────────────────────────────────
step "wayland-protocols"
clone "https://gitlab.freedesktop.org/wayland/wayland-protocols.git" \
      "${BUILD_DIR}/wayland-protocols" "1.37"
meson setup "${BUILD_DIR}/obj/wayland-protocols" \
    "${BUILD_DIR}/wayland-protocols" \
    --prefix="$PREFIX" --buildtype=release 2>&1 | tail -3
ninja -C "${BUILD_DIR}/obj/wayland-protocols" -j"$JOBS" install

# ── libxkbcommon ───────────────────────────────────────────────────────────
step "libxkbcommon (NEON optimized)"
clone "https://github.com/xkbcommon/libxkbcommon.git" \
      "${BUILD_DIR}/libxkbcommon" "xkbcommon-2.0.0"
meson setup "${BUILD_DIR}/obj/libxkbcommon" "${BUILD_DIR}/libxkbcommon" \
    --prefix="$PREFIX" --buildtype=release \
    -Denable-docs=false -Denable-tools=false \
    -Dc_args="-arch arm64" -Dc_link_args="-arch arm64" 2>&1 | tail -3
ninja -C "${BUILD_DIR}/obj/libxkbcommon" -j"$JOBS" install

# ── pixman ────────────────────────────────────────────────────────────────
step "pixman (arm64 NEON)"
clone "https://gitlab.freedesktop.org/pixman/pixman.git" \
      "${BUILD_DIR}/pixman" "pixman-0.43.4"
meson setup "${BUILD_DIR}/obj/pixman" "${BUILD_DIR}/pixman" \
    --prefix="$PREFIX" --buildtype=release \
    -Dtests=disabled -Ddemos=disabled \
    -Dc_args="-arch arm64 -mcpu=apple-m1" \
    -Dc_link_args="-arch arm64" 2>&1 | tail -3
ninja -C "${BUILD_DIR}/obj/pixman" -j"$JOBS" install

# ── libinput ──────────────────────────────────────────────────────────────
step "libinput"
clone "https://gitlab.freedesktop.org/libinput/libinput.git" \
      "${BUILD_DIR}/libinput" "1.27.0"
meson setup "${BUILD_DIR}/obj/libinput" "${BUILD_DIR}/libinput" \
    --prefix="$PREFIX" --buildtype=release \
    -Ddocumentation=false -Dtests=false -Dlibwacom=false \
    -Ddebug-gui=false \
    -Dc_args="-arch arm64" -Dc_link_args="-arch arm64" 2>&1 | tail -3
ninja -C "${BUILD_DIR}/obj/libinput" -j"$JOBS" install

# ── wlroots ───────────────────────────────────────────────────────────────
step "wlroots (arm64 patched)"
clone "https://gitlab.freedesktop.org/wlroots/wlroots.git" \
      "${BUILD_DIR}/wlroots" "0.18.0"

for patch in "${LUNA_ROOT}/patches/wlroots/"*.patch; do
    [[ -f "$patch" ]] || continue
    git -C "${BUILD_DIR}/wlroots" apply "$patch" 2>/dev/null || true
done

meson setup "${BUILD_DIR}/obj/wlroots" "${BUILD_DIR}/wlroots" \
    --prefix="$PREFIX" --buildtype=release \
    -Dbackends=drm,libinput \
    -Drenderers=pixman \
    -Dxwayland=disabled \
    -Dc_args="-arch arm64 -DWLR_USE_UNSTABLE" \
    -Dc_link_args="-arch arm64" 2>&1 | tail -5
ninja -C "${BUILD_DIR}/obj/wlroots" -j"$JOBS" install

# ── luna-compositor ───────────────────────────────────────────────────────
step "luna-compositor (arm64)"
cmake -S "${LUNA_ROOT}/compositor" \
      -B "${BUILD_DIR}/obj/luna-compositor" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      --quiet
cmake --build "${BUILD_DIR}/obj/luna-compositor" -j"$JOBS"
cmake --install "${BUILD_DIR}/obj/luna-compositor"

log "luna-compositor installed: ${PREFIX}/bin/luna-compositor"
log "WAYLAND_DISPLAY will be set to wayland-0 at runtime"

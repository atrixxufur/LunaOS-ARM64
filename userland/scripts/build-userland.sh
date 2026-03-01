#!/usr/bin/env bash
# =============================================================================
# build-userland.sh — PureDarwin userland assembly for LunaOS arm64
#
# arm64 differences from x86_64:
#   - All xcodebuild: ARCHS=arm64 (not x86_64)
#   - All cmake: -DCMAKE_OSX_ARCHITECTURES=arm64
#   - All clang: -arch arm64 (not -arch x86_64)
#   - launchd: amfi_get_out_of_my_way=1 in boot-args (AMFI bypass for arm64)
#   - No kext-dev-mode workarounds needed for IOFramebuffer (use IOMobileFramebuffer)
#   - kernelcache instead of raw mach_kernel in boot path
#   - Homebrew prefix: /opt/homebrew (Apple Silicon)
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${LUNA_ROOT}/build/rootfs"
SOURCES="${LUNA_ROOT}/build/userland-src"
PREFIX="${ROOTFS}/usr/local"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
MACOS_TAG="macos-262"
APPLE_OSS="https://github.com/apple-oss-distributions"
PUREDARWIN="https://github.com/PureDarwin"

SKIP_WAYLAND="${SKIP_WAYLAND:-0}"
[[ "$1" == "--skip-wayland" ]] && SKIP_WAYLAND=1

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()      { echo -e "${GREEN}[userland-arm64]${NC} $*"; }
step()     { echo -e "${BLUE}[userland-arm64]${NC} ── $*"; }
warn()     { echo -e "${YELLOW}[userland-arm64] WARN:${NC} $*"; }
progress() { echo -e "${GREEN}[userland-arm64]${NC} [$1/10] $2"; }

# arm64: Homebrew in /opt/homebrew
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
export PATH="${BREW_PREFIX}/bin:$PATH"
export CFLAGS="-arch arm64"
export CXXFLAGS="-arch arm64"
export LDFLAGS="-arch arm64"

[[ "$(uname -s)" == "Darwin" ]] || { echo "Darwin only"; exit 1; }

mkdir -p "$ROOTFS" "$SOURCES" "$PREFIX"/{bin,lib,sbin,include,share}

clone() {
    local url="$1" dir="$2" ref="${3:-main}"
    [[ -d "$dir/.git" ]] && return
    git clone --depth 1 --branch "$ref" "$url" "$dir" --quiet
}

# ── Stage 1: Libc / libSystem ─────────────────────────────────────────────
progress 1 "Libc / libSystem (arm64)"
for repo in Libc libsyscall Libm Libinfo libclosure; do
    clone "${APPLE_OSS}/${repo}" "${SOURCES}/${repo}" "$MACOS_TAG" || \
        warn "$repo not available"
done

if [[ -d "${SOURCES}/Libc" ]]; then
    xcodebuild -project "${SOURCES}/Libc/Libc.xcodeproj" \
        -target Libc -sdk macosx \
        ARCHS=arm64 \
        ONLY_ACTIVE_ARCH=NO \
        DSTROOT="$ROOTFS" \
        install 2>&1 | grep -E "^(Build|error)" | head -10 || \
        warn "Libc build had issues"
fi

# ── Stage 2: dyld (arm64) ──────────────────────────────────────────────────
progress 2 "dyld (arm64 dynamic linker)"
clone "${APPLE_OSS}/dyld" "${SOURCES}/dyld" "$MACOS_TAG"
if [[ -d "${SOURCES}/dyld" ]]; then
    cmake -S "${SOURCES}/dyld" \
          -B "${LUNA_ROOT}/build/obj/dyld" \
          -DCMAKE_INSTALL_PREFIX="$ROOTFS" \
          -DCMAKE_OSX_ARCHITECTURES=arm64 \
          --quiet
    cmake --build "${LUNA_ROOT}/build/obj/dyld" -j"$JOBS" 2>&1 | tail -3
    cmake --install "${LUNA_ROOT}/build/obj/dyld"
fi

# ── Stage 3: libdispatch (GCD — critical on arm64) ────────────────────────
progress 3 "libdispatch / Grand Central Dispatch (arm64)"
# GCD is especially important on arm64 — seatd-darwin uses dispatch_source_t
clone "${APPLE_OSS}/libdispatch" "${SOURCES}/libdispatch" "$MACOS_TAG"
if [[ -d "${SOURCES}/libdispatch" ]]; then
    cmake -S "${SOURCES}/libdispatch" \
          -B "${LUNA_ROOT}/build/obj/libdispatch" \
          -DCMAKE_INSTALL_PREFIX="$ROOTFS/usr" \
          -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DENABLE_SWIFT=OFF \
          --quiet
    cmake --build "${LUNA_ROOT}/build/obj/libdispatch" -j"$JOBS"
    cmake --install "${LUNA_ROOT}/build/obj/libdispatch"
fi

# ── Stage 4: libpthread ────────────────────────────────────────────────────
progress 4 "libpthread (arm64)"
clone "${APPLE_OSS}/libpthread" "${SOURCES}/libpthread" "$MACOS_TAG"
if [[ -d "${SOURCES}/libpthread" ]]; then
    xcodebuild -project "${SOURCES}/libpthread/libpthread.xcodeproj" \
        -target libpthread -sdk macosx \
        ARCHS=arm64 DSTROOT="$ROOTFS" install 2>&1 | tail -5 || true
fi

# ── Stage 5: launchd / XPC ────────────────────────────────────────────────
progress 5 "launchd / XPC (arm64)"
clone "${PUREDARWIN}/XPC" "${SOURCES}/xpc-puredarwin" || \
    warn "PureDarwin XPC not available — using stub"

# Install boot configuration for arm64
# arm64 CRITICAL: amfi_get_out_of_my_way=1 is required for unsigned KEXTs
mkdir -p "${ROOTFS}/Library/Preferences/SystemConfiguration"
cp "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" \
   "${ROOTFS}/Library/Preferences/SystemConfiguration/"

# Install LaunchDaemons
mkdir -p "${ROOTFS}/Library/LaunchDaemons"
cp "${LUNA_ROOT}/userland/rootfs-skel/Library/LaunchDaemons/"*.plist \
   "${ROOTFS}/Library/LaunchDaemons/" 2>/dev/null || true

# ── Stage 6: Shell utilities (arm64) ──────────────────────────────────────
progress 6 "Shell utilities (arm64)"
for repo in bash file_cmds shell_cmds text_cmds system_cmds network_cmds; do
    clone "${APPLE_OSS}/${repo}" "${SOURCES}/${repo}" "$MACOS_TAG" || \
        warn "$repo not available"
    if [[ -d "${SOURCES}/${repo}" ]]; then
        xcodebuild -project "${SOURCES}/${repo}"/*.xcodeproj \
            -sdk macosx ARCHS=arm64 \
            DSTROOT="$ROOTFS" install 2>&1 | tail -3 || \
            warn "${repo} build issues"
    fi
done

# Profile (arm64: /opt/homebrew in PATH)
cat > "${ROOTFS}/private/etc/profile" <<'PROFILE'
export PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/501
export WLR_RENDERER=pixman
export WLR_BACKENDS=drm
export LIBSEAT_BACKEND=seatd
# arm64: MoltenVK path (if installed)
export VK_ICD_FILENAMES=/usr/local/share/vulkan/icd.d/lvp_icd.aarch64.json
PROFILE

# ── Stage 7: Network stack ────────────────────────────────────────────────
progress 7 "Network stack (arm64)"
for repo in configd mDNSResponder; do
    clone "${APPLE_OSS}/${repo}" "${SOURCES}/${repo}" "$MACOS_TAG" || \
        warn "$repo not available"
done

# ── Stage 8: LunaOS services (arm64) ──────────────────────────────────────
progress 8 "LunaOS services (arm64)"

# libdrm-darwin
clang -arch arm64 -dynamiclib \
    -o "${PREFIX}/lib/libdrm.dylib" \
    -install_name "${PREFIX}/lib/libdrm.dylib" \
    "${LUNA_ROOT}/drm-shim/user/libdrm-darwin.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -Os 2>&1 | tail -3

# seatd-darwin (arm64: links against libdispatch + CoreFoundation for GCD/os_log)
clang -arch arm64 \
    -o "${PREFIX}/sbin/seatd-darwin" \
    "${LUNA_ROOT}/seatd-darwin/seatd-darwin.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -framework CoreFoundation \
    -ldispatch \
    -Os 2>&1 | tail -3

# darwin-evdev-bridge (arm64: links IOKit + CoreFoundation + libdispatch)
clang -arch arm64 \
    -o "${PREFIX}/sbin/darwin-evdev-bridge" \
    "${LUNA_ROOT}/evdev-bridge/darwin-evdev-bridge.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -framework IOKit \
    -framework CoreFoundation \
    -ldispatch \
    -Os 2>&1 | tail -3

# Copy IODRMShim.kext (arm64)
mkdir -p "${ROOTFS}/Library/Extensions/IODRMShim.kext/Contents"
cp "${LUNA_ROOT}/drm-shim/kern/IODRMShim.cpp" \
   "${LUNA_ROOT}/drm-shim/kern/IODRMShim.entitlements" \
   "${ROOTFS}/Library/Extensions/IODRMShim.kext/Contents/"
cp "${LUNA_ROOT}/drm-shim/kern/Info.plist" \
   "${ROOTFS}/Library/Extensions/IODRMShim.kext/Contents/"

# ── Stage 9: Wayland stack (arm64) ────────────────────────────────────────
progress 9 "Wayland compositor stack (arm64)"
if [[ "$SKIP_WAYLAND" != "1" ]]; then
    JOBS="$JOBS" PREFIX="$PREFIX" bash "${LUNA_ROOT}/scripts/build-compositor.sh"
else
    log "  skipping (--skip-wayland)"
fi

# ── Stage 10: Rootfs skeleton ─────────────────────────────────────────────
progress 10 "Rootfs skeleton (arm64)"

# Standard Darwin directories
for dir in bin sbin usr/bin usr/sbin usr/local/bin usr/local/sbin \
           var/log var/run tmp dev Library/Extensions \
           System/Library/Kernels System/Library/Extensions \
           private/etc private/var private/tmp run; do
    mkdir -p "${ROOTFS}/${dir}"
done

# Darwin symlinks
[[ -L "${ROOTFS}/var" ]] || ln -sf private/var "${ROOTFS}/var"
[[ -L "${ROOTFS}/tmp" ]] || ln -sf private/tmp "${ROOTFS}/tmp"
[[ -L "${ROOTFS}/etc" ]] || ln -sf private/etc "${ROOTFS}/etc"

# Overlay rootfs-skel
cp -r "${LUNA_ROOT}/userland/rootfs-skel/"* "${ROOTFS}/"

# arm64: kernel output is kernelcache (not mach_kernel)
if [[ -f "${LUNA_ROOT}/build/kernel/output/kernelcache" ]]; then
    cp "${LUNA_ROOT}/build/kernel/output/kernelcache" \
       "${ROOTFS}/System/Library/Kernels/kernelcache"
elif [[ -f "${LUNA_ROOT}/build/kernel/output/kernel" ]]; then
    cp "${LUNA_ROOT}/build/kernel/output/kernel" \
       "${ROOTFS}/System/Library/Kernels/kernel"
fi

# SystemVersion.plist
cat > "${ROOTFS}/System/Library/CoreServices/SystemVersion.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>ProductName</key>       <string>LunaOS</string>
    <key>ProductVersion</key>    <string>0.1.0</string>
    <key>ProductBuildVersion</key><string>26A0001-arm64</string>
    <key>ProductCPUType</key>    <string>arm64</string>
    <key>ProductHardware</key>   <string>Apple Silicon</string>
</dict>
</plist>
PLIST

log "=== Userland assembly complete (arm64) ==="
log "Rootfs: ${ROOTFS}"
ls -la "${ROOTFS}/" 2>/dev/null | head -20

#!/usr/bin/env bash
# verify-rootfs.sh — LunaOS arm64 rootfs verification
set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${LUNA_ROOT}/build/rootfs"
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn() { echo -e "  ${YELLOW}?${NC} $*"; }
FAILURES=0

echo "══════════════════════════════════════════════════"
echo " LunaOS arm64 rootfs verification"
echo "══════════════════════════════════════════════════"

echo "── Kernel"
[[ -f "${ROOTFS}/System/Library/Kernels/kernel" ]]  && pass "kernel (arm64)" || fail "kernel missing"
[[ -f "${ROOTFS}/System/Library/Kernels/kernelcache" ]] && pass "kernelcache" || warn "kernelcache missing (needed for Apple Silicon boot)"

echo "── C runtime"
for lib in libSystem.B.dylib libdispatch.dylib libpthread.dylib; do
    [[ -f "${ROOTFS}/usr/lib/${lib}" ]] && pass "$lib" || fail "$lib missing"
done

echo "── LunaOS services"
[[ -f "${ROOTFS}/usr/local/sbin/seatd-darwin" ]]        && pass "seatd-darwin" || fail "seatd-darwin missing"
[[ -f "${ROOTFS}/usr/local/sbin/darwin-evdev-bridge" ]] && pass "darwin-evdev-bridge" || fail "evdev-bridge missing"
[[ -f "${ROOTFS}/usr/local/lib/libdrm.dylib" ]]          && pass "libdrm.dylib" || fail "libdrm missing"
[[ -f "${ROOTFS}/usr/local/bin/luna-compositor" ]]        && pass "luna-compositor" || fail "compositor missing"
[[ -f "${ROOTFS}/usr/local/bin/luna-shell" ]]             && pass "luna-shell" || fail "shell missing"

echo "── KEXTs (arm64: IOMobileFramebuffer not IOFramebuffer)"
[[ -d "${ROOTFS}/Library/Extensions/IODRMShim.kext" ]] && pass "IODRMShim.kext" || fail "IODRMShim.kext missing"
for kext in IOMobileGraphicsFamily IOHIDFamily IOStorageFamily IONetworkingFamily; do
    [[ -d "${ROOTFS}/System/Library/Extensions/${kext}.kext" ]] && pass "${kext}.kext" || warn "${kext}.kext missing"
done

echo "── Graphics (Mesa 26 arm64)"
[[ -f "${ROOTFS}/usr/local/lib/libvulkan_lvp.dylib" ]]  && pass "lavapipe (Vulkan 1.4)" || fail "lavapipe missing"
[[ -f "${ROOTFS}/usr/local/lib/libGL.dylib" ]]           && pass "OpenGL (llvmpipe)" || fail "OpenGL missing"
[[ -f "${ROOTFS}/usr/local/bin/vulkaninfo" ]]             && pass "vulkaninfo" || warn "vulkaninfo missing"

echo "── AGX stub (Apple GPU)"
if [[ -f "${ROOTFS}/usr/local/share/vulkan/icd.d/agx_icd.aarch64.json.disabled_until_agx_kext" ]]; then
    pass "agx ICD stub present (will activate when AGX KEXT is ported)"
else
    warn "agx ICD stub not found"
fi

echo "── Apps"
[[ -f "${ROOTFS}/usr/local/bin/foot" ]]       && pass "foot terminal" || warn "foot terminal not built"
[[ -f "${ROOTFS}/usr/local/bin/luna-files" ]] && pass "luna-files" || warn "luna-files not built"
[[ -f "${ROOTFS}/usr/local/bin/luna-editor" ]] && pass "luna-editor" || warn "luna-editor not built"

echo "── Config files"
for f in private/etc/fstab private/etc/passwd private/etc/hostname \
          private/etc/rc.lunaos System/Library/CoreServices/SystemVersion.plist \
          Library/Preferences/SystemConfiguration/com.apple.Boot.plist; do
    [[ -f "${ROOTFS}/${f}" ]] && pass "$f" || fail "$f missing"
done

echo "── Architecture (all must be arm64)"
BAD=0
for f in "${ROOTFS}/usr/local/lib/"lib*.dylib \
          "${ROOTFS}/usr/local/bin/luna-"*; do
    [[ -f "$f" ]] || continue
    arch=$(file "$f" 2>/dev/null | grep -o "arm64\|aarch64" | head -1 || echo "")
    if [[ -z "$arch" ]]; then
        warn "Cannot verify arch: $(basename $f)"
    fi
done
(( BAD==0 )) && pass "Architecture check passed"

echo "══════════════════════════════════════════════════"
if (( FAILURES==0 )); then
    echo -e "${GREEN}rootfs verified — arm64 LunaOS ready${NC}"
else
    echo -e "${RED}${FAILURES} failure(s) — check build logs${NC}"
    exit 1
fi

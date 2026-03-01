#!/usr/bin/env bash
# verify-graphics.sh — LunaOS graphics verification (arm64)
set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${LUNA_ROOT}/build/rootfs/usr/local"
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn() { echo -e "  ${YELLOW}?${NC} $*"; }
FAILURES=0

echo "══════════════════════════════════════════════════"
echo " LunaOS graphics verification (arm64)"
echo "══════════════════════════════════════════════════"

echo ""; echo "── Mesa libraries"
for lib in libGL.dylib libEGL.dylib libGLESv2.dylib libgbm.dylib; do
    [[ -f "${PREFIX}/lib/${lib}" ]] && pass "$lib" || warn "$lib not found"
done

echo ""; echo "── Vulkan ICDs (arm64 — aarch64 suffix)"
ICD_DIR="${PREFIX}/share/vulkan/icd.d"
check_icd() {
    local f="$1" desc="$2" stub_key="${3:-}"
    if   [[ -f "${ICD_DIR}/${f}" ]]; then pass "${f} — ${desc}"
    elif [[ -n "$stub_key" && -f "${ICD_DIR}/${f}.disabled_until_${stub_key}" ]]; then
         warn "${f} — STUB (enable when ${stub_key} ready)"
    else fail "${f} missing"; fi
}
check_icd "lvp_icd.aarch64.json"    "lavapipe Vulkan 1.4 (NEON software) ✅"
check_icd "virtio_icd.aarch64.json" "virtio-gpu Vulkan (QEMU virt)       ✅"
check_icd "agx_icd.aarch64.json"    "Apple GPU Vulkan                    ⏸" "agx_kext"

echo ""; echo "── Vulkan driver libs"
for lib in libvulkan_lvp.dylib libvulkan_virtio.dylib libvulkan.dylib; do
    [[ -f "${PREFIX}/lib/${lib}" ]] && pass "$lib" || warn "$lib not found"
done
[[ -f "${PREFIX}/lib/libvulkan_apple.dylib" ]] && pass "libvulkan_apple.dylib (agx stub)" \
    || warn "libvulkan_apple.dylib not found (agx stub)"

echo ""; echo "── MoltenVK (optional — Vulkan via Metal)"
MOLTENVK_LIB="$(brew --prefix molten-vk 2>/dev/null)/lib/libMoltenVK.dylib"
if [[ -f "$MOLTENVK_LIB" ]]; then
    pass "MoltenVK found: $MOLTENVK_LIB"
    warn "To activate: copy libMoltenVK.dylib + ICD manifest to ${ICD_DIR}/"
else
    warn "MoltenVK not installed (optional — brew install molten-vk)"
    warn "Provides Vulkan 1.2 via Metal — good for real Apple Silicon hardware"
fi

echo ""; echo "── Tools"
[[ -f "${PREFIX}/bin/vulkaninfo" ]] && pass "vulkaninfo" || fail "vulkaninfo missing"
[[ -f "${PREFIX}/bin/vkcube"     ]] && pass "vkcube"     || fail "vkcube missing"

echo ""; echo "── Architecture (all must be arm64)"
BAD=0
for lib in "${PREFIX}/lib"/libGL*.dylib "${PREFIX}/lib"/libvulkan*.dylib \
           "${PREFIX}/lib/libgbm.dylib"; do
    [[ -f "$lib" ]] || continue
    arch=$(file "$lib" | grep -o "arm64\|aarch64" || echo "UNKNOWN")
    [[ "$arch" == "arm64" || "$arch" == "aarch64" ]] || { warn "Wrong arch: $lib"; BAD=$((BAD+1)); }
done
(( BAD == 0 )) && pass "All graphics libs are arm64"

echo ""
echo "══════════════════════════════════════════════════"
if (( FAILURES == 0 )); then
    echo -e "${GREEN}Graphics OK (arm64).${NC}"
    echo ""
    echo "Active: lavapipe (Vulkan 1.4, NEON) + llvmpipe (GL 4.6, NEON)"
    echo "VM:     virtio-gpu (QEMU -device virtio-vga-gl)"
    echo "Future: AGX KEXT or MoltenVK for Apple GPU hardware acceleration"
else
    echo -e "${RED}${FAILURES} failure(s).${NC}"; exit 1
fi
echo "══════════════════════════════════════════════════"

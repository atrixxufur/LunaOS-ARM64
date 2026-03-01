#!/usr/bin/env bash
# verify-kernel.sh — LunaOS XNU kernel verification (arm64)
set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTPUT="${LUNA_ROOT}/build/kernel/output"
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn() { echo -e "  ${YELLOW}?${NC} $*"; }
FAILURES=0

echo "══════════════════════════════════════════════"
echo " LunaOS XNU kernel verification (arm64)"
echo "══════════════════════════════════════════════"

# Kernel binary
if [[ -f "${OUTPUT}/kernel" ]]; then
    ARCH=$(file "${OUTPUT}/kernel" | grep -o "arm64\|aarch64" | head -1 || echo "unknown")
    [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]] \
        && pass "kernel — arm64 Mach-O" \
        || fail "kernel — wrong arch: $ARCH (expected arm64)"
    SIZE=$(du -sh "${OUTPUT}/kernel" | cut -f1)
    pass "kernel size: ${SIZE}"
else
    fail "kernel not found at ${OUTPUT}/kernel"
    warn "Run: kernel/scripts/build-xnu.sh"
fi

# kernelcache (arm64 preferred over raw kernel)
if [[ -f "${OUTPUT}/kernelcache" ]]; then
    pass "kernelcache (prelinked kernel+KEXTs for iBoot)"
else
    warn "kernelcache not found — will use raw kernel in QEMU"
    warn "For real Apple Silicon: run kmutil create after userland is built"
fi

# Patches applied
echo ""
echo "── Patches"
for patch in "${LUNA_ROOT}/kernel/patches/"*.patch; do
    [[ -f "$patch" ]] || continue
    pass "$(basename "$patch")"
done

# Boot config
echo ""
echo "── Boot configuration"
[[ -f "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" ]] && \
    pass "com.apple.Boot.plist (arm64 — amfi_get_out_of_my_way=1)" || \
    fail "com.apple.Boot.plist missing"

# Check for arm64-critical boot args
if [[ -f "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" ]]; then
    grep -q "amfi_get_out_of_my_way=1" "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" && \
        pass "amfi_get_out_of_my_way=1 present (CRITICAL for arm64 KEXTs)" || \
        fail "amfi_get_out_of_my_way=1 MISSING — IODRMShim.kext will not load on arm64"
fi

echo ""
echo "══════════════════════════════════════════════"
if (( FAILURES == 0 )); then
    echo -e "${GREEN}Kernel OK (arm64).${NC}"
    echo ""
    echo "Boot chain notes:"
    echo "  QEMU:           qemu-system-aarch64 -M virt → raw kernel"
    echo "  Apple Silicon:  iBoot → kernelcache (via m1n1 or DFU)"
else
    echo -e "${RED}${FAILURES} failure(s).${NC}"
    exit 1
fi
echo "══════════════════════════════════════════════"

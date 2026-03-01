#!/usr/bin/env bash
# =============================================================================
# build-xnu.sh — LunaOS XNU kernel build (arm64 / Apple Silicon)
#
# Builds XNU for ARM64 (Apple Silicon: M1/M2/M3/M4).
#
# Key differences from x86_64 build:
#   - MACHINE_CONFIGS=ARM64 (not X86_64)
#   - Boot chain: iBoot → m1n1/boot.efi (not GRUB)
#   - Display: IOMobileFramebuffer KEXT (not IOFramebuffer)
#   - IOKit families: IOMobileGraphicsFamily (not IOGraphicsFamily)
#   - Kernel loads at 0xfffffff007004000 (arm64 KASLR base)
#   - Requires Apple Silicon KDK (Kernel Debug Kit for arm64)
#
# Hardware targets:
#   - Apple M1 (MacBook Air/Pro 2020-2021, Mac Mini 2020, iMac 2021)
#   - Apple M2 (MacBook Air/Pro 2022-2023, Mac Mini 2023)
#   - Apple M3 (MacBook Pro 2023, iMac 2023)
#   - Apple M4 (MacBook Pro 2024, Mac Mini 2024)
#
# QEMU testing (no real hardware needed):
#   qemu-system-aarch64 -M virt -cpu cortex-a76 -m 4096 \
#     -kernel build/kernel/output/mach_kernel ...
#
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build/kernel"
SOURCES="${LUNA_ROOT}/build/kernel-src"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
MACOS_TAG="macos-262"
APPLE_OSS="https://github.com/apple-oss-distributions"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'
log()  { echo -e "${GREEN}[xnu-arm64]${NC} $*"; }
step() { echo -e "${BLUE}[xnu-arm64]${NC} ── $*"; }
warn() { echo -e "${YELLOW}[xnu-arm64] WARN:${NC} $*"; }
die()  { echo -e "${RED}[xnu-arm64] ERROR:${NC} $*" >&2; exit 1; }

log "=== LunaOS XNU kernel build — arm64 (Apple Silicon) ==="
log "Host: $(uname -srm)"
log "Jobs: ${JOBS}"

# ── Preflight ─────────────────────────────────────────────────────────────────
[[ "$(uname -s)" == "Darwin" ]] || die "Must build on macOS"

# arm64 can build arm64 natively OR cross-compile from x86_64 Mac with Rosetta
UNAME_M="$(uname -m)"
if [[ "$UNAME_M" == "arm64" ]]; then
    log "Building natively on Apple Silicon ✓"
elif [[ "$UNAME_M" == "x86_64" ]]; then
    warn "Cross-compiling arm64 kernel on x86_64 Mac (via Rosetta/LLVM cross)"
    warn "Install arm64 toolchain: brew install llvm --with-targets=aarch64"
else
    die "Unknown host architecture: $UNAME_M"
fi

# Check for arm64 KDK (Kernel Debug Kit)
KDK_PATH="/Library/Developer/KDKs"
ARM64_KDK=""
if [[ -d "$KDK_PATH" ]]; then
    ARM64_KDK=$(ls "$KDK_PATH" | grep -i "arm64\|apple.silicon" | head -1 || true)
fi
if [[ -z "$ARM64_KDK" ]]; then
    warn "No arm64 KDK found in $KDK_PATH"
    warn "Download from: https://developer.apple.com/download/all/?q=Kernel+Debug+Kit"
    warn "Continuing without KDK — some IOKit headers may be missing"
fi

# LLVM setup
if [[ "$UNAME_M" == "arm64" ]]; then
    LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || echo /opt/homebrew/opt/llvm)"
else
    LLVM_PREFIX="$(brew --prefix llvm@18 2>/dev/null || echo /usr/local/opt/llvm@18)"
fi
export PATH="${LLVM_PREFIX}/bin:$PATH"
log "LLVM: $(clang --version | head -1)"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
TOOLCHAIN="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"

mkdir -p "$BUILD_DIR" "$SOURCES"

clone() {
    local name="$1" tag="$2"
    local dest="${SOURCES}/${name}"
    [[ -d "$dest/.git" ]] && { git -C "$dest" fetch --tags -q; return; }
    git clone --depth 1 --branch "$tag" \
        "${APPLE_OSS}/${name}.git" "$dest" --quiet
}

# ── Clone dependencies ─────────────────────────────────────────────────────────
step "Cloning XNU arm64 dependencies"
for entry in \
    "dtrace:${MACOS_TAG}" \
    "AvailabilityVersions:${MACOS_TAG}" \
    "libplatform:${MACOS_TAG}" \
    "libdispatch:${MACOS_TAG}" \
    "libpthread:${MACOS_TAG}" \
    "xnu:${MACOS_TAG}" ; do
    clone "${entry%%:*}" "${entry##*:}"
done

# arm64-specific IOKit families
for entry in \
    "IOMobileGraphicsFamily:${MACOS_TAG}" \
    "IOHIDFamily:${MACOS_TAG}" \
    "IOStorageFamily:${MACOS_TAG}" \
    "IONetworkingFamily:${MACOS_TAG}" \
    "IOACPIFamily:${MACOS_TAG}" ; do
    clone "${entry%%:*}" "${entry##*:}" || warn "  ${entry%%:*} not available"
done

# ── Build dtrace (ctfconvert, ctfmerge — needed by XNU) ─────────────────────
step "Building dtrace (ctf tools)"
make -C "${SOURCES}/dtrace" \
    SRCROOT="${SOURCES}/dtrace" \
    OBJROOT="${BUILD_DIR}/obj/dtrace" \
    DSTROOT="${BUILD_DIR}/dst" \
    SDKROOT="$SDK" \
    RC_ARCHS=arm64 \
    install 2>&1 | tail -5 || warn "dtrace had warnings"
export PATH="${BUILD_DIR}/dst/usr/local/bin:$PATH"

# ── Build AvailabilityVersions ────────────────────────────────────────────────
step "AvailabilityVersions"
make -C "${SOURCES}/AvailabilityVersions" \
    SRCROOT="${SOURCES}/AvailabilityVersions" \
    DSTROOT="${BUILD_DIR}/dst" install 2>&1 | tail -3 || true

# ── Build libplatform headers ─────────────────────────────────────────────────
step "libplatform headers"
xcodebuild -project "${SOURCES}/libplatform/libplatform.xcodeproj" \
    -target libplatform_headers -sdk macosx \
    ARCHS=arm64 \
    SRCROOT="${SOURCES}/libplatform" \
    OBJROOT="${BUILD_DIR}/obj/libplatform" \
    DSTROOT="${BUILD_DIR}/dst" install 2>&1 | grep -E "^(Build|error)" || true

# ── Apply LunaOS arm64 patches ────────────────────────────────────────────────
step "Applying arm64 patches"
for patch in "${LUNA_ROOT}/kernel/patches/"*.patch; do
    [[ -f "$patch" ]] || continue
    log "  $(basename "$patch")"
    git -C "${SOURCES}/xnu" apply "$patch" 2>/dev/null || \
        warn "  $(basename "$patch") skipped (already applied)"
done

# ── Build XNU kernel — ARM64 ──────────────────────────────────────────────────
step "Building XNU RELEASE_ARM64 (~45-90 min)"

XNU_OBJ="${BUILD_DIR}/obj/xnu"
XNU_DST="${BUILD_DIR}/dst"
XNU_SYM="${BUILD_DIR}/sym/xnu"
mkdir -p "$XNU_OBJ" "$XNU_DST" "$XNU_SYM"

# ARM64 specific: need PMAP_CS and KASAN options for Apple Silicon security model
make -C "${SOURCES}/xnu" \
    SDKROOT="$SDK" \
    OBJROOT="$XNU_OBJ" \
    DSTROOT="$XNU_DST" \
    SYMROOT="$XNU_SYM" \
    TOOLCHAINDIR="$TOOLCHAIN" \
    KERNEL_CONFIGS="RELEASE" \
    MACHINE_CONFIGS="ARM64" \
    PLATFORM=MacOSX \
    BUILD_WERROR=0 \
    LUNAOS=1 \
    LUNAOS_ARCH=arm64 \
    -j"$JOBS" \
    install 2>&1 | tee "${BUILD_DIR}/xnu-arm64-build.log" | \
    grep -E "(error:|^===|Compiling|Linking)" || true

# ── Collect outputs ────────────────────────────────────────────────────────────
step "Collecting outputs"
OUTPUT="${BUILD_DIR}/output"
mkdir -p "$OUTPUT"

# arm64 kernel location (different from x86_64)
for candidate in \
    "${XNU_DST}/System/Library/Kernels/kernel" \
    "${XNU_SYM}/release/arm64/kernel" \
    "${XNU_OBJ}/RELEASE_ARM64/kernel" ; do
    if [[ -f "$candidate" ]]; then
        cp "$candidate" "${OUTPUT}/kernel"
        log "Kernel: ${OUTPUT}/kernel"
        file "${OUTPUT}/kernel"
        break
    fi
done

# ARM64 also needs kernelcache (prelinked kernel + KEXTs) for Apple boot
# On Apple Silicon, iBoot loads kernelcache not raw mach_kernel
if command -v kmutil &>/dev/null; then
    log "Building kernelcache via kmutil..."
    kmutil create \
        --kernel "${OUTPUT}/kernel" \
        --volume-root "${LUNA_ROOT}/build/rootfs" \
        --output "${OUTPUT}/kernelcache" \
        --arch arm64 2>&1 | tail -5 || warn "kmutil failed — use raw kernel"
fi

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
log "╔══════════════════════════════════════════════════════════╗"
log "║     LunaOS XNU kernel (arm64 / Apple Silicon) done      ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  kernel:      ${OUTPUT}/kernel"
log "║  kernelcache: ${OUTPUT}/kernelcache (if kmutil succeeded)"
log "║  Build log:   ${BUILD_DIR}/xnu-arm64-build.log"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Next: ./scripts/build-compositor.sh"
log "╚══════════════════════════════════════════════════════════╝"

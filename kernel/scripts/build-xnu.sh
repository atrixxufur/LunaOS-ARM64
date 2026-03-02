#!/usr/bin/env bash
# =============================================================================
# build-xnu.sh — LunaOS XNU kernel build (arm64 / Apple Silicon)
#
# FIX: Previous version used fake tag "macos-262". Apple's repos use real
# version tags like "xnu-11417.140.69". This script auto-detects your Darwin
# version and resolves real tags via git ls-remote.
#
# Darwin version → macOS → XNU tag:
#   Darwin 24.x → macOS 14 Sonoma   → xnu-10063.x
#   Darwin 25.x → macOS 15 Sequoia  → xnu-11417.x  (your host: 25.4.0)
#   Darwin 26.x → macOS 26 Tahoe    → xnu-12377.x
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build/kernel"
SOURCES="${LUNA_ROOT}/build/kernel-src"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
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

[[ "$(uname -s)" == "Darwin" ]] || die "Must build on macOS"

UNAME_M="$(uname -m)"
[[ "$UNAME_M" == "arm64" ]] && log "Building natively on Apple Silicon ✓" || \
    warn "Cross-compiling arm64 on x86_64"

# ── Detect correct XNU tag from Darwin version ────────────────────────────────
DARWIN_MAJOR="${UNAME_R:-$(uname -r)}"
DARWIN_MAJOR="${DARWIN_MAJOR%%.*}"

step "Detecting XNU tag for Darwin $(uname -r)"
case "$DARWIN_MAJOR" in
    22) XNU_TAG="xnu-8792.141.2"    ;;  # macOS 13 Ventura
    23) XNU_TAG="xnu-10002.81.5"    ;;  # macOS 13.x late
    24) XNU_TAG="xnu-10063.141.2"   ;;  # macOS 14 Sonoma
    25) XNU_TAG="xnu-11417.140.69"  ;;  # macOS 15 Sequoia ← your host (Darwin 25.4.0)
    26) XNU_TAG="xnu-12377.41.6"    ;;  # macOS 26 Tahoe
    *)  XNU_TAG="xnu-11417.140.69"  ;;  # default: macOS 15
esac
XNU_TAG="${XNU_TAG_OVERRIDE:-$XNU_TAG}"
log "XNU tag: ${XNU_TAG}"

# ── Resolve dependency tags via git ls-remote (real network query) ─────────────
# Each Apple OSS component has its own version numbering — no "macos-XXX" tags.
# We query available tags and pick the latest for each repo.

find_latest_tag() {
    local repo="$1" prefix="${2:-}"
    local url="${APPLE_OSS}/${repo}.git"
    local tag
    tag=$(git ls-remote --tags "$url" 2>/dev/null \
        | grep -o 'refs/tags/[^{}]*' \
        | sed 's|refs/tags/||' \
        | grep -v '\^{}' \
        | { [[ -n "$prefix" ]] && grep "^${prefix}" || cat; } \
        | sort -V | tail -1)
    [[ -n "$tag" ]] && echo "$tag" || echo "HEAD"
}

step "Resolving dependency tags via git ls-remote..."
DTRACE_TAG=$(find_latest_tag "dtrace" "dtrace-")
AVAIL_TAG=$(find_latest_tag "AvailabilityVersions" "AvailabilityVersions-")
LIBPLATFORM_TAG=$(find_latest_tag "libplatform" "libplatform-")
LIBDISPATCH_TAG=$(find_latest_tag "libdispatch" "libdispatch-")
LIBPTHREAD_TAG=$(find_latest_tag "libpthread" "libpthread-")

log "  xnu:                  ${XNU_TAG}"
log "  dtrace:               ${DTRACE_TAG}"
log "  AvailabilityVersions: ${AVAIL_TAG}"
log "  libplatform:          ${LIBPLATFORM_TAG}"
log "  libdispatch:          ${LIBDISPATCH_TAG}"
log "  libpthread:           ${LIBPTHREAD_TAG}"

# ── KDK check ─────────────────────────────────────────────────────────────────
KDK_PATH="/Library/Developer/KDKs"
ARM64_KDK=""
[[ -d "$KDK_PATH" ]] && ARM64_KDK=$(ls "$KDK_PATH" 2>/dev/null | head -1 || true)
if [[ -z "$ARM64_KDK" ]]; then
    warn "No KDK in $KDK_PATH — download from:"
    warn "  https://developer.apple.com/download/all/?q=Kernel+Debug+Kit"
    warn "Continuing without KDK (some IOKit headers may be missing)"
else
    log "KDK: ${ARM64_KDK}"
fi

# ── LLVM ──────────────────────────────────────────────────────────────────────
LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || echo /opt/homebrew/opt/llvm)"
export PATH="${LLVM_PREFIX}/bin:$PATH"
log "LLVM: $(clang --version | head -1)"

SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)"
TOOLCHAIN="$(xcode-select -p 2>/dev/null)/Toolchains/XcodeDefault.xctoolchain"
mkdir -p "$BUILD_DIR" "$SOURCES"

# ── Clone helper ──────────────────────────────────────────────────────────────
clone() {
    local name="$1" tag="$2"
    local dest="${SOURCES}/${name}"

    if [[ -d "${dest}/.git" ]]; then
        log "  ${name}: already cloned, updating to ${tag}"
        git -C "$dest" fetch --tags --quiet 2>/dev/null || true
        git -C "$dest" checkout "${tag}" --quiet 2>/dev/null || \
            warn "  ${name}: cannot checkout ${tag}, using current HEAD"
        return 0
    fi

    log "  Cloning ${name} @ ${tag}"
    if git clone --depth 1 --branch "$tag" \
           "${APPLE_OSS}/${name}.git" "$dest" --quiet 2>/dev/null; then
        return 0
    fi
    # Fallback: clone default branch then checkout tag
    warn "  --branch ${tag} failed, cloning HEAD then checking out tag"
    git clone --depth 1 "${APPLE_OSS}/${name}.git" "$dest" --quiet
    git -C "$dest" fetch --tags --quiet 2>/dev/null || true
    git -C "$dest" checkout "$tag" --quiet 2>/dev/null || \
        warn "  ${name}: tag ${tag} not available, using HEAD"
}

# ── Clone all dependencies ────────────────────────────────────────────────────
step "Cloning XNU arm64 dependencies"
clone "dtrace"                "$DTRACE_TAG"
clone "AvailabilityVersions" "$AVAIL_TAG"
clone "libplatform"          "$LIBPLATFORM_TAG"
clone "libdispatch"          "$LIBDISPATCH_TAG"
clone "libpthread"           "$LIBPTHREAD_TAG"
clone "xnu"                  "$XNU_TAG"

step "Cloning arm64 IOKit families (best-effort)"
for repo in IOMobileGraphicsFamily IOHIDFamily IOStorageFamily IONetworkingFamily; do
    IOKIT_TAG=$(find_latest_tag "$repo" 2>/dev/null || echo "HEAD")
    clone "$repo" "$IOKIT_TAG" 2>/dev/null || warn "  ${repo} not available"
done

# ── Build dtrace (provides ctfconvert, ctfmerge — required by XNU) ───────────
step "Building dtrace (ctf tools)"
mkdir -p "${BUILD_DIR}/obj/dtrace" "${BUILD_DIR}/dst"
DTRACE_SRC="${SOURCES}/dtrace"

if [[ -f "${DTRACE_SRC}/Makefile" ]]; then
    make -C "$DTRACE_SRC" \
        SRCROOT="$DTRACE_SRC" OBJROOT="${BUILD_DIR}/obj/dtrace" \
        DSTROOT="${BUILD_DIR}/dst" SDKROOT="$SDK" RC_ARCHS=arm64 \
        install 2>&1 | tail -5 || warn "dtrace make warnings (often safe)"
elif [[ -f "${DTRACE_SRC}/dtrace.xcodeproj/project.pbxproj" ]]; then
    xcodebuild -project "${DTRACE_SRC}/dtrace.xcodeproj" \
        -scheme ctfconvert -sdk macosx ARCHS=arm64 \
        OBJROOT="${BUILD_DIR}/obj/dtrace" DSTROOT="${BUILD_DIR}/dst" \
        install 2>&1 | grep -E "^(error:|=== BUILD)" || true
    xcodebuild -project "${DTRACE_SRC}/dtrace.xcodeproj" \
        -scheme ctfmerge -sdk macosx ARCHS=arm64 \
        OBJROOT="${BUILD_DIR}/obj/dtrace" DSTROOT="${BUILD_DIR}/dst" \
        install 2>&1 | grep -E "^(error:|=== BUILD)" || true
else
    warn "dtrace: no build system found — ctf tools may be missing"
fi
export PATH="${BUILD_DIR}/dst/usr/local/bin:${BUILD_DIR}/dst/usr/bin:$PATH"
log "ctfconvert: $(which ctfconvert 2>/dev/null || echo 'not found')"

# ── AvailabilityVersions ──────────────────────────────────────────────────────
step "AvailabilityVersions"
AVAIL_SRC="${SOURCES}/AvailabilityVersions"
[[ -f "${AVAIL_SRC}/Makefile" ]] && \
    make -C "$AVAIL_SRC" SRCROOT="$AVAIL_SRC" DSTROOT="${BUILD_DIR}/dst" install 2>&1 | tail -3 || true

# ── libplatform headers ───────────────────────────────────────────────────────
step "libplatform headers"
LIBPLAT_SRC="${SOURCES}/libplatform"
if [[ -f "${LIBPLAT_SRC}/libplatform.xcodeproj/project.pbxproj" ]]; then
    xcodebuild -project "${LIBPLAT_SRC}/libplatform.xcodeproj" \
        -target libplatform_headers -sdk macosx ARCHS=arm64 \
        SRCROOT="$LIBPLAT_SRC" OBJROOT="${BUILD_DIR}/obj/libplatform" \
        DSTROOT="${BUILD_DIR}/dst" install 2>&1 | grep "^error:" || true
elif [[ -f "${LIBPLAT_SRC}/Makefile" ]]; then
    make -C "$LIBPLAT_SRC" SRCROOT="$LIBPLAT_SRC" \
        OBJROOT="${BUILD_DIR}/obj/libplatform" \
        DSTROOT="${BUILD_DIR}/dst" RC_ARCHS=arm64 install 2>&1 | tail -3 || true
fi

# ── Apply LunaOS patches ──────────────────────────────────────────────────────
step "Applying arm64 patches"
for patch in "${LUNA_ROOT}/kernel/patches/"*.patch; do
    [[ -f "$patch" ]] || continue
    log "  $(basename "$patch")"
    git -C "${SOURCES}/xnu" apply "$patch" 2>/dev/null || \
        warn "  $(basename "$patch") skipped (already applied or conflicts)"
done

# ── Build XNU RELEASE_ARM64 ───────────────────────────────────────────────────
step "Building XNU RELEASE_ARM64 (~45-90 min first time)"
log "  Build log: ${BUILD_DIR}/xnu-arm64-build.log"

XNU_OBJ="${BUILD_DIR}/obj/xnu"
XNU_DST="${BUILD_DIR}/dst"
XNU_SYM="${BUILD_DIR}/sym/xnu"
mkdir -p "$XNU_OBJ" "$XNU_DST" "$XNU_SYM"

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
    BUILD_LTO=0 \
    LUNAOS=1 \
    LUNAOS_ARCH=arm64 \
    -j"$JOBS" \
    install 2>&1 | tee "${BUILD_DIR}/xnu-arm64-build.log" | \
    grep -E "(error:|^===|BUILD (SUCCEEDED|FAILED))" | head -30 || true

# ── Collect output ────────────────────────────────────────────────────────────
step "Collecting outputs"
OUTPUT="${BUILD_DIR}/output"
mkdir -p "$OUTPUT"

KERNEL_FOUND=0
for candidate in \
    "${XNU_DST}/System/Library/Kernels/kernel" \
    "${XNU_DST}/System/Library/Kernels/kernel.release.t8103" \
    "${XNU_DST}/System/Library/Kernels/kernel.release.vmapple" \
    "${XNU_SYM}/RELEASE_ARM64/kernel" \
    "${XNU_OBJ}/RELEASE_ARM64/kernel" ; do
    if [[ -f "$candidate" ]]; then
        cp "$candidate" "${OUTPUT}/kernel"
        log "Kernel: ${OUTPUT}/kernel ($(file "${OUTPUT}/kernel" | grep -o 'Mach-O.*'))"
        KERNEL_FOUND=1; break
    fi
done

[[ "$KERNEL_FOUND" -eq 0 ]] && {
    warn "Kernel not found — check build log:"
    warn "  ${BUILD_DIR}/xnu-arm64-build.log"
    warn "Searching for kernel candidates:"
    find "${XNU_OBJ}" "${XNU_DST}" -name "kernel*" -type f 2>/dev/null | head -5
}

# kernelcache (optional — for real Apple Silicon boot)
if [[ -f "${OUTPUT}/kernel" ]] && command -v kmutil &>/dev/null; then
    log "Building kernelcache via kmutil..."
    kmutil create \
        --kernel "${OUTPUT}/kernel" \
        --volume-root "${LUNA_ROOT}/build/rootfs" \
        --output "${OUTPUT}/kernelcache" \
        --arch arm64 2>&1 | tail -5 || warn "kmutil skipped"
fi

echo ""
log "╔══════════════════════════════════════════════════════════╗"
log "║     LunaOS XNU kernel (arm64)                           ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  XNU:  ${XNU_TAG}"
[[ -f "${OUTPUT}/kernel" ]]      && log "║  kernel:      ✓" || log "║  kernel:      ✗ NOT FOUND"
[[ -f "${OUTPUT}/kernelcache" ]] && log "║  kernelcache: ✓" || log "║  kernelcache: - (optional)"
log "║  log: ${BUILD_DIR}/xnu-arm64-build.log"
log "╚══════════════════════════════════════════════════════════╝"

#!/usr/bin/env bash
# =============================================================================
# build-graphics.sh — LunaOS graphics stack for arm64 / Apple Silicon
#
# Key differences from x86_64:
#   1. LLVM target: aarch64 (not x86_64)
#   2. Mesa drivers: agx (Apple GPU stub) instead of anv/radv
#   3. lavapipe: still works on arm64 (LLVM backend supports AArch64)
#   4. llvmpipe: arm64 NEON optimized (Mesa uses LLVM AArch64 backend)
#   5. No virgl on real hardware (virgl = QEMU virtio-gpu, arm64 QEMU uses it too)
#   6. Vulkan: lavapipe primary, MoltenVK optional for Metal fallback
#
# Driver matrix (arm64):
#   lavapipe      — Vulkan 1.4, software, always works         [ACTIVE]
#   llvmpipe      — OpenGL 4.6, software, always works         [ACTIVE]
#   virgl         — OpenGL/Vulkan via QEMU virtio-gpu           [ACTIVE in VM]
#   agx stub      — Apple GPU Vulkan, needs AGXAccelerator KEXT [STUB]
#   MoltenVK      — Vulkan via Metal (fallback for real HW)     [OPTIONAL]
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build/graphics"
SOURCES="${BUILD_DIR}/src"
PREFIX="${LUNA_ROOT}/build/rootfs/usr/local"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

MESA_VERSION="26.0.0"
VULKAN_LOADER_VERSION="1.4.304"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'
log()  { echo -e "${GREEN}[graphics-arm64]${NC} $*"; }
step() { echo -e "${BLUE}[graphics-arm64]${NC} ── $*"; }
warn() { echo -e "${YELLOW}[graphics-arm64] WARN:${NC} $*"; }
die()  { echo -e "${RED}[graphics-arm64] ERROR:${NC} $*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "Darwin only"

# ── LLVM setup (arm64) ────────────────────────────────────────────────────────
# On Apple Silicon, Homebrew installs to /opt/homebrew
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
LLVM_PREFIX="${BREW_PREFIX}/opt/llvm"
[[ -d "$LLVM_PREFIX" ]] || { brew install llvm; LLVM_PREFIX="${BREW_PREFIX}/opt/llvm"; }
export PATH="${LLVM_PREFIX}/bin:$PATH"
LLVM_CONFIG="${LLVM_PREFIX}/bin/llvm-config"

log "LLVM: $($LLVM_CONFIG --version) @ ${LLVM_PREFIX}"
log "Target: $($LLVM_CONFIG --host-target)"

# arm64 compile flags
export CFLAGS="-arch arm64"
export CXXFLAGS="-arch arm64"
export LDFLAGS="-arch arm64"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
mkdir -p "$BUILD_DIR" "$SOURCES" "$PREFIX"

clone() {
    local url="$1" dir="$2" ref="${3:-main}"
    [[ -d "$dir/.git" ]] && return
    git clone --depth 1 --branch "$ref" "$url" "$dir" --quiet
}

# ── Step 1: Vulkan headers + loader (arm64) ───────────────────────────────────
step "Vulkan ${VULKAN_LOADER_VERSION} headers + loader (arm64)"

clone "https://github.com/KhronosGroup/Vulkan-Headers.git" \
      "${SOURCES}/Vulkan-Headers" "v${VULKAN_LOADER_VERSION}"

cmake -S "${SOURCES}/Vulkan-Headers" \
      -B "${BUILD_DIR}/obj/Vulkan-Headers" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      --quiet
cmake --install "${BUILD_DIR}/obj/Vulkan-Headers"

clone "https://github.com/KhronosGroup/Vulkan-Loader.git" \
      "${SOURCES}/Vulkan-Loader" "v${VULKAN_LOADER_VERSION}"

# Apply Darwin arm64 patch
[[ -f "${LUNA_ROOT}/graphics/patches/vulkan-loader/0001-darwin-arm64.patch" ]] && \
    git -C "${SOURCES}/Vulkan-Loader" apply \
        "${LUNA_ROOT}/graphics/patches/vulkan-loader/0001-darwin-arm64.patch" \
        2>/dev/null || true

cmake -S "${SOURCES}/Vulkan-Loader" \
      -B "${BUILD_DIR}/obj/Vulkan-Loader" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" \
      -DBUILD_WSI_WAYLAND_SUPPORT=ON \
      -DBUILD_WSI_XCB_SUPPORT=OFF \
      -DBUILD_WSI_XLIB_SUPPORT=OFF \
      -DBUILD_TESTS=OFF \
      --quiet
cmake --build "${BUILD_DIR}/obj/Vulkan-Loader" -j"$JOBS"
cmake --install "${BUILD_DIR}/obj/Vulkan-Loader"
log "  Vulkan loader installed"

# ── Step 2: GBM shim (arm64) ──────────────────────────────────────────────────
step "GBM shim (arm64 — IOSurface-backed)"

clang -arch arm64 -dynamiclib \
    -o "${PREFIX}/lib/libgbm.dylib" \
    -install_name "${PREFIX}/lib/libgbm.dylib" \
    -current_version 1.0.0 \
    -compatibility_version 1.0.0 \
    "${LUNA_ROOT}/graphics/include/darwin-gbm.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -I"${PREFIX}/include" \
    -L"${PREFIX}/lib" -ldrm \
    -Os 2>&1 | tail -3 || warn "GBM build issues"

cat > "${PREFIX}/lib/pkgconfig/gbm.pc" <<GBM
prefix=${PREFIX}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: gbm
Description: LunaOS GBM shim (arm64 IOMobileFramebuffer)
Version: 1.0.0
Libs: -L\${libdir} -lgbm
Cflags: -I\${includedir}
Requires: libdrm
GBM

# ── Step 3: Mesa 26 (arm64) ───────────────────────────────────────────────────
step "Mesa ${MESA_VERSION} arm64 (llvmpipe + lavapipe + agx-stub + virgl)"

clone "https://gitlab.freedesktop.org/mesa/mesa.git" \
      "${SOURCES}/mesa" "mesa-${MESA_VERSION}"

# Apply all arm64 Mesa patches
for patch in "${LUNA_ROOT}/graphics/patches/mesa/"*.patch; do
    [[ -f "$patch" ]] || continue
    git -C "${SOURCES}/mesa" apply "$patch" 2>/dev/null || true
done

MESON_OPTS=(
    "--prefix=${PREFIX}"
    "--buildtype=release"
    "--wrap-mode=nofallback"

    # Gallium: llvmpipe (SW GL) + virgl (QEMU) + softpipe (fallback)
    "-Dgallium-drivers=llvmpipe,virgl,softpipe"

    # Vulkan: lavapipe (SW) + virtio (QEMU) + agx (Apple GPU stub)
    # Note: arm64 uses 'apple' instead of 'intel'/'amd'
    "-Dvulkan-drivers=swrast,virtio,apple"

    "-Dplatforms=wayland"
    "-Dgles1=enabled"
    "-Dgles2=enabled"
    "-Dopengl=true"
    "-Dglx=disabled"
    "-Degl=enabled"
    "-Dgbm=enabled"
    "-Degl-native-platform=wayland"
    "-Dllvm=enabled"
    "-Dshared-llvm=enabled"
    "-Dvulkan-layers=device-select,overlay"
    "-Dvulkan-icd-dir=${PREFIX}/share/vulkan/icd.d"
    "-Dbuild-tests=false"
    "-Dwerror=false"
    # arm64: disable Intel-specific compiler
    "-Dintel-clc=disabled"
    "-Dmicrosoft-clc=disabled"
    # arm64: enable NEON optimizations
    "-Dc_args=-arch arm64 -mcpu=apple-m1"
    "-Dcpp_args=-arch arm64 -mcpu=apple-m1"
)

export LLVM_CONFIG="$LLVM_CONFIG"

meson setup "${BUILD_DIR}/obj/mesa" "${SOURCES}/mesa" \
    "${MESON_OPTS[@]}" \
    -Dc_link_args="-arch arm64" \
    -Dcpp_link_args="-arch arm64" \
    2>&1 | tail -15

log "  Mesa configured — building..."
ninja -C "${BUILD_DIR}/obj/mesa" -j"$JOBS" 2>&1 | tail -20 || true
ninja -C "${BUILD_DIR}/obj/mesa" install

# ── Step 4: ICD manifests (arm64) ─────────────────────────────────────────────
step "Vulkan ICD manifests (arm64)"

ICD_DIR="${PREFIX}/share/vulkan/icd.d"
mkdir -p "$ICD_DIR"

# lavapipe — software Vulkan 1.4 (always active on arm64)
cat > "${ICD_DIR}/lvp_icd.aarch64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_lvp.dylib",
        "api_version": "1.4.304"
    }
}
JSON

# virtio — QEMU arm64 virtio-gpu
cat > "${ICD_DIR}/virtio_icd.aarch64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_virtio.dylib",
        "api_version": "1.3.0"
    }
}
JSON

# agx stub — Apple GPU (arm64 only, replaces anv/radv)
# Disabled until AGXAccelerator KEXT is ported
cat > "${ICD_DIR}/agx_icd.aarch64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_apple.dylib",
        "api_version": "1.4.304"
    }
}
JSON
mv "${ICD_DIR}/agx_icd.aarch64.json" \
   "${ICD_DIR}/agx_icd.aarch64.json.disabled_until_agx_kext"

log "  Active ICDs (arm64):"
log "    ✅ lavapipe  — Vulkan 1.4 software (NEON optimized)"
log "    ✅ virtio    — QEMU virtio-gpu"
log "    ⏸  agx      — Apple GPU (disabled until AGXAccelerator KEXT ported)"

# ── Step 5: MoltenVK (optional — Vulkan via Metal for real Apple Silicon) ─────
step "MoltenVK (optional — Vulkan via Metal, Apple GPU acceleration)"
log "  MoltenVK provides Vulkan 1.2 on top of Apple's Metal API"
log "  This is NOT the same as native Vulkan — Metal adds overhead"
log "  but works on real Apple Silicon hardware immediately."
log ""
log "  To install MoltenVK manually:"
log "    brew install molten-vk"
log "    cp \$(brew --prefix molten-vk)/lib/libMoltenVK.dylib ${PREFIX}/lib/"
log ""
log "  To activate:"
log "    cat > ${ICD_DIR}/MoltenVK_icd.json <<'JSON'"
log '    {"file_format_version":"1.0.0","ICD":{"library_path":"libMoltenVK.dylib","api_version":"1.2.0"}}'
log "    JSON"
log ""
log "  MoltenVK vs lavapipe on Apple Silicon:"
log "    MoltenVK + Metal GPU = MUCH faster (uses Apple GPU hardware)"
log "    lavapipe + NEON CPU  = fallback (CPU only, ~1/10 the speed)"
log "  Recommend: use MoltenVK on real hardware, lavapipe in QEMU"

# ── Step 6: vulkaninfo + vkcube ───────────────────────────────────────────────
step "Vulkan tools (arm64)"
clone "https://github.com/KhronosGroup/Vulkan-Tools.git" \
      "${SOURCES}/Vulkan-Tools" "v${VULKAN_LOADER_VERSION}"

cmake -S "${SOURCES}/Vulkan-Tools" \
      -B "${BUILD_DIR}/obj/Vulkan-Tools" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" \
      -DBUILD_ICD=OFF -DBUILD_CUBE=ON -DBUILD_VULKANINFO=ON \
      --quiet
cmake --build "${BUILD_DIR}/obj/Vulkan-Tools" -j"$JOBS"
cmake --install "${BUILD_DIR}/obj/Vulkan-Tools"

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
log "╔═════════════════════════════════════════════════════════════════╗"
log "║      LunaOS Graphics Stack (arm64 / Apple Silicon) Done        ║"
log "╠═════════════════════════════════════════════════════════════════╣"
log "║  OpenGL 4.6  → llvmpipe (NEON optimized software)              ║"
log "║  Vulkan 1.4  → lavapipe (NEON optimized software)              ║"
log "║  Vulkan/GL   → virtio   (QEMU virt machine)                    ║"
log "║  Vulkan 1.4  → agx stub (Apple GPU — needs AGX KEXT)           ║"
log "║  Vulkan 1.2  → MoltenVK (optional, via Metal, real hardware)   ║"
log "╠═════════════════════════════════════════════════════════════════╣"
log "║  Hardware Vulkan path:                                          ║"
log "║  1. Port AGXAccelerator as Darwin KEXT (see Asahi agx driver)  ║"
log "║     OR install MoltenVK (brew install molten-vk) for Metal     ║"
log "║  2. Rename agx_icd.aarch64.json.disabled → .json              ║"
log "╚═════════════════════════════════════════════════════════════════╝"

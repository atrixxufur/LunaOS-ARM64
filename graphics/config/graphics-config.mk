# graphics-config.mk — LunaOS graphics (arm64 / Apple Silicon)

MESA_VERSION           := 26.0.0
LLVM_VERSION           := latest          # arm64: just use brew's default llvm
VULKAN_LOADER_VERSION  := 1.4.304

# arm64: Homebrew prefix is /opt/homebrew on Apple Silicon
BREW_PREFIX   := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
LLVM_PREFIX   := $(BREW_PREFIX)/opt/llvm
LLVM_CONFIG   := $(LLVM_PREFIX)/bin/llvm-config

# ── arm64 Driver matrix ────────────────────────────────────────────────────────
#
# Gallium (OpenGL):
#   llvmpipe  — software OpenGL 4.6, NEON optimized   [ACTIVE]
#   virgl     — QEMU virtio-gpu OpenGL                [ACTIVE in VM]
#   softpipe  — pure software fallback                [FALLBACK]
#
# Vulkan:
#   lavapipe  — software Vulkan 1.4, NEON optimized   [ACTIVE]
#   virtio    — QEMU virtio-gpu Vulkan                [ACTIVE in VM]
#   agx stub  — Apple GPU Vulkan                      [STUB — needs AGX KEXT]
#   MoltenVK  — Vulkan via Metal (optional, real HW)  [OPTIONAL]
#
GALLIUM_DRIVERS := llvmpipe,virgl,softpipe
VULKAN_DRIVERS  := swrast,virtio,apple    # 'apple' = agx in Mesa

# arm64 ICD suffix (different from x86_64)
ICD_SUFFIX      := aarch64

GRAPHICS_PREFIX := $(LUNA_ROOT)/build/rootfs/usr/local

# ── Enabling hardware Vulkan (arm64 options) ───────────────────────────────────
#
# Option A — AGX KEXT (best, native Vulkan):
#   Port Asahi Linux's drm-asahi as Darwin IOKit KEXT
#   Reference: https://github.com/AsahiLinux/linux (drivers/gpu/drm/asahi)
#   Then rename: agx_icd.aarch64.json.disabled → agx_icd.aarch64.json
#
# Option B — MoltenVK (easier, works NOW):
#   brew install molten-vk
#   Copies libMoltenVK.dylib + ICD manifest into place
#   Provides Vulkan 1.2 over Metal — good performance on Apple GPU
#   Not true native Vulkan but viable for desktop apps
#
# QEMU testing (no hardware needed):
#   qemu-system-aarch64 -M virt -device virtio-vga-gl -display sdl,gl=on
#   virtio ICD activates automatically

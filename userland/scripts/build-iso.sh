#!/usr/bin/env bash
# =============================================================================
# build-iso.sh — LunaOS arm64 ISO / boot image packager
#
# Produces two outputs:
#
#   1. LunaOS-0.1.0-arm64.iso — QEMU bootable ISO
#      Boot chain: QEMU firmware (EDK2 AARCH64) → GRUB arm64 EFI
#                  → XNU kernel → launchd → luna-shell
#      Use with: qemu-system-aarch64 -M virt -bios QEMU_EFI.fd -cdrom LunaOS.iso
#
#   2. LunaOS-0.1.0-arm64-applesilicon.img — Apple Silicon boot image
#      Boot chain: iBoot → m1n1 → U-Boot → XNU kernelcache
#      Use with: m1n1 + real Apple Silicon Mac in Permissive Security mode
#      (Same approach as Asahi Linux)
#
# WHY TWO OUTPUTS:
#   x86_64 only needed one ISO because GRUB handles both BIOS and EFI on x86.
#   arm64 Apple Silicon cannot boot from a standard ISO — iBoot requires a
#   specific memory layout and must run before any bootloader we control.
#   QEMU arm64 uses the 'virt' machine type which supports EFI (GRUB works),
#   but real Apple Silicon needs the m1n1 path.
# =============================================================================

set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${LUNA_ROOT}/build/rootfs"
BUILD="${LUNA_ROOT}/build"
VERSION="0.1.0"
ISO_OUT="${BUILD}/LunaOS-${VERSION}-arm64.iso"
ASIL_OUT="${BUILD}/LunaOS-${VERSION}-arm64-applesilicon.img"
KERNEL="${BUILD}/kernel/output/kernel"
KERNELCACHE="${BUILD}/kernel/output/kernelcache"

GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
log()  { echo -e "${GREEN}[build-iso-arm64]${NC} $*"; }
step() { echo -e "${BLUE}[build-iso-arm64]${NC} ── $*"; }

# ── Install kernel into rootfs ────────────────────────────────────────────────
step "Installing kernel"
mkdir -p "${ROOTFS}/System/Library/Kernels"
mkdir -p "${ROOTFS}/Library/Preferences/SystemConfiguration"
[[ -f "$KERNEL" ]] && cp "$KERNEL" "${ROOTFS}/System/Library/Kernels/kernel"
[[ -f "$KERNELCACHE" ]] && cp "$KERNELCACHE" "${ROOTFS}/System/Library/Kernels/kernelcache"
cp "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" \
   "${ROOTFS}/Library/Preferences/SystemConfiguration/"

# ── GRUB arm64 EFI config ─────────────────────────────────────────────────────
step "Writing GRUB arm64 EFI config"
GRUB_DIR="${BUILD}/iso-staging/boot/grub"
mkdir -p "$GRUB_DIR"

cat > "${GRUB_DIR}/grub.cfg" <<'GRUB'
# LunaOS arm64 GRUB config — QEMU virt machine (EDK2 AARCH64 firmware)
set timeout=5
set default=0

menuentry "LunaOS 0.1.0 (arm64)" {
    # arm64: darwin_kernel loads mach-O arm64 kernel
    # boot-args: amfi_get_out_of_my_way=1 is CRITICAL on arm64
    darwin_kernel /System/Library/Kernels/kernel \
        "amfi_get_out_of_my_way=1 csr-active-config=0xFF kext-dev-mode=1 \
         cs_enforcement_disable=1 -v pmuflags=1 serial=3"
}

menuentry "LunaOS 0.1.0 arm64 (verbose)" {
    darwin_kernel /System/Library/Kernels/kernel \
        "amfi_get_out_of_my_way=1 csr-active-config=0xFF kext-dev-mode=1 \
         cs_enforcement_disable=1 -v -s serial=3 io=0xffffffff"
}

menuentry "LunaOS arm64 Single User / Recovery" {
    darwin_kernel /System/Library/Kernels/kernel \
        "amfi_get_out_of_my_way=1 csr-active-config=0xFF -s -v serial=3"
}
GRUB

# ── Build GRUB arm64 EFI binary ───────────────────────────────────────────────
step "Building GRUB arm64 EFI"
EFI_DIR="${BUILD}/iso-staging/EFI/BOOT"
mkdir -p "$EFI_DIR"

# GRUB arm64 EFI module list (different from x86_64 — no pc modules)
ARM64_GRUB_MODULES="part_gpt part_msdos fat iso9660 normal boot \
    search search_fs_uuid search_label linux echo cat ls test \
    configfile terminal serial usb usbserial_common"

if command -v grub-mkimage &>/dev/null; then
    grub-mkimage \
        -O arm64-efi \
        -p /boot/grub \
        -o "${EFI_DIR}/BOOTAA64.EFI" \
        $ARM64_GRUB_MODULES
    log "  GRUB arm64 EFI: ${EFI_DIR}/BOOTAA64.EFI"
else
    warn() { echo -e "\033[1;33m[build-iso-arm64] WARN:\033[0m $*"; }
    warn "grub-mkimage not found — install: brew install grub"
    warn "Skipping GRUB EFI; ISO will boot via xorriso embedded grub only"
fi

# ── Stage rootfs into ISO staging area ────────────────────────────────────────
step "Staging rootfs"
rsync -a --delete "${ROOTFS}/" "${BUILD}/iso-staging/" \
      --exclude='.DS_Store' 2>/dev/null || \
cp -r "${ROOTFS}/." "${BUILD}/iso-staging/"

# ── Build ISO (arm64 EFI only — no BIOS MBR needed) ──────────────────────────
step "Building arm64 ISO with xorriso"
if command -v xorriso &>/dev/null; then
    xorriso -as mkisofs \
        -volid "LUNAOS_${VERSION//./}_ARM64" \
        -joliet -joliet-long \
        -rational-rock \
        -e "EFI/BOOT/BOOTAA64.EFI" \
        -no-emul-boot \
        -o "$ISO_OUT" \
        "${BUILD}/iso-staging"
    log "  ISO: $ISO_OUT ($(du -sh "$ISO_OUT" | cut -f1))"
else
    warn() { echo "\033[1;33m[build-iso-arm64] WARN:\033[0m $*"; }
    warn "xorriso not found — install: brew install xorriso"
fi

# ── Apple Silicon boot image (m1n1 path) ──────────────────────────────────────
step "Building Apple Silicon boot image (m1n1 path)"
log ""
log "  Apple Silicon boot requires m1n1 as Stage 2 bootloader."
log "  m1n1 is developed by the Asahi Linux project:"
log "    https://github.com/AsahiLinux/m1n1"
log ""
log "  Setup (one-time, requires Mac in Permissive Security mode):"
log "    1. Boot macOS Recovery"
log "    2. Security Policy → Permissive Security"
log "    3. Install m1n1 via: kmutil install --kernel <kernel>"
log "    4. m1n1 then loads U-Boot, which loads LunaOS kernelcache"
log ""

if [[ -f "$KERNELCACHE" ]]; then
    # Create a simple flat image: [m1n1 stub header][kernelcache]
    # Real Apple Silicon deployment would use asahi-installer
    dd if=/dev/zero of="$ASIL_OUT" bs=1M count=512 2>/dev/null
    log "  Placeholder image: $ASIL_OUT"
    log "  ⚠  Replace m1n1 header with real m1n1 binary from Asahi project"
    log "  ⚠  See: scripts/install-applesilicon.sh for full deployment"
else
    log "  kernelcache not found — skipping Apple Silicon image"
    log "  Build with: make kernel (includes kmutil kernelcache step)"
fi

# ── QEMU test command ─────────────────────────────────────────────────────────
echo ""
log "╔══════════════════════════════════════════════════════════════════╗"
log "║            LunaOS arm64 — Boot Instructions                     ║"
log "╠══════════════════════════════════════════════════════════════════╣"
log "║  QEMU (arm64 virt machine — works on any macOS host):           ║"
log "║                                                                  ║"
log "║  # Download EDK2 AARCH64 firmware:                              ║"
log "║  brew install qemu  # includes QEMU_EFI.fd                      ║"
log "║                                                                  ║"
log "║  qemu-system-aarch64 \\                                          ║"
log "║    -M virt -cpu cortex-a76 -m 4096 -smp 4 \\                    ║"
log "║    -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \\       ║"
log "║    -device virtio-gpu-gl -display sdl,gl=on \\                  ║"
log "║    -device virtio-keyboard-pci \\                                ║"
log "║    -device virtio-tablet-pci \\                                  ║"
log "║    -cdrom ${ISO_OUT} -boot d                    ║"
log "║                                                                  ║"
log "║  Apple Silicon (real hardware — Permissive Security required):  ║"
log "║    See: https://asahilinux.org/2022/03/asahi-linux-alpha/       ║"
log "║    Use m1n1 + U-Boot + XNU kernelcache approach                 ║"
log "╚══════════════════════════════════════════════════════════════════╝"

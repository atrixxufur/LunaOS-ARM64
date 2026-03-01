# LunaOS arm64 — Darwin/XNU + Wayland for Apple Silicon

A bootable Darwin OS targeting arm64 Apple Silicon (M1/M2/M3/M4 Macs),
replacing Quartz Compositor with a custom Wayland stack.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Apps: foot terminal · luna-files · luna-editor                  │
├──────────────────────────────────────────────────────────────────┤
│  Desktop shell: luna-shell (Retina 2x, wlr-layer-shell)         │
├──────────────────────────────────────────────────────────────────┤
│  Compositor: luna-compositor (wlroots, pixman, lavapipe NEON)   │
├──────────────────────────────────────────────────────────────────┤
│  Graphics: Mesa 26 · Vulkan 1.4 lavapipe · agx stub             │
├──────────────────────────────────────────────────────────────────┤
│  Glue: IODRMShim.kext · seatd-darwin (GCD) · evdev-bridge       │
├──────────────────────────────────────────────────────────────────┤
│  Kernel: XNU macos-262 (RELEASE_ARM64) · kernelcache            │
├──────────────────────────────────────────────────────────────────┤
│  Hardware: Apple Silicon M1/M2/M3/M4 · IOMobileFramebuffer      │
└──────────────────────────────────────────────────────────────────┘
```

## Key arm64 differences vs x86_64

| Component | x86_64 | arm64 |
|---|---|---|
| Display KEXT | IOFramebuffer | **IOMobileFramebuffer** |
| Boot loader | GRUB2 + EFI | **iBoot → boot.efi** |
| Boot image | ISO 9660 | **GPT HFS+ .dmg** |
| AMFI bypass | kext-dev-mode=1 | **amfi_get_out_of_my_way=1** |
| GPU stub | anv (Intel) + radv (AMD) | **agx (Apple GPU)** |
| GPU instant path | virgl (QEMU) | **virgl + MoltenVK option** |
| Retina | N/A | **2x buffer_scale** |
| Homebrew | /usr/local | **/opt/homebrew** |
| QEMU machine | pc/q35 | **virt -cpu cortex-a76** |
| Arch flags | -arch x86_64 | **-arch arm64** |

## Quick start

```bash
# Build everything
make all

# Check status
make status

# Test in QEMU (no real hardware needed)
make qemu
```

## Hardware requirements

- **Apple Silicon Mac**: M1, M2, M3, or M4
- **macOS** host for building (Xcode 15+, KDK for arm64)
- **Permissive Security** mode (set via Startup Security Utility)
- **nvram** boot-args must include `amfi_get_out_of_my_way=1`

### One-time hardware setup
```bash
# Set boot args (allows unsigned KEXTs on Apple Silicon)
sudo nvram boot-args="amfi_get_out_of_my_way=1 csr-active-config=0xFF kext-dev-mode=1 -v"

# Startup Security Utility → Reduced Security → Allow user management of KEXTs
# (Restart into Recovery OS: hold power button until "Loading startup options")
```

## Vulkan on real Apple Silicon

The stack ships with:
- **lavapipe** (always active) — software Vulkan via LLVM AArch64/NEON
- **agx stub** (disabled) — placeholder for Apple GPU Vulkan

For real hardware GPU acceleration before the agx KEXT is ported:
```bash
# Install MoltenVK (Vulkan via Metal — real Apple GPU, no KEXT needed)
brew install molten-vk
cp $(brew --prefix molten-vk)/lib/libMoltenVK.dylib \
   build/rootfs/usr/local/lib/

# Activate MoltenVK ICD
cat > build/rootfs/usr/local/share/vulkan/icd.d/MoltenVK_icd.json <<'JSON'
{"file_format_version":"1.0.0","ICD":{"library_path":"libMoltenVK.dylib","api_version":"1.2.0"}}
JSON
```

MoltenVK gives ~10x the performance of lavapipe on Apple Silicon by
routing Vulkan calls through Apple's Metal GPU API.

## Boot chain

```
Power button
  → iBoot (Apple ROM, cannot modify)
    → boot.efi (from EFI partition)
      → kernelcache (XNU arm64 + IODRMShim.kext)
        → launchd
          → /etc/rc.lunaos
            → seatd-darwin → darwin-evdev-bridge → luna-compositor
              → luna-shell (Retina 2x panel + launcher + wallpaper)
```

## File layout

```
lunaos-arm64/
├── Makefile                    ← make all / make qemu
├── kernel/
│   ├── scripts/build-xnu.sh   ← RELEASE_ARM64 build
│   ├── patches/                ← IOMobileFramebuffer + CSR + AMFI
│   └── config/com.apple.Boot.plist  ← amfi_get_out_of_my_way=1
├── drm-shim/
│   ├── kern/IODRMShim.cpp      ← IOMobileFramebuffer → /dev/dri/card0
│   └── user/libdrm-darwin.c   ← userspace DRM API (arm64 64B alignment)
├── evdev-bridge/               ← IOHIDFamily → evdev (multitouch support)
├── seatd-darwin/               ← GCD-based seat manager
├── compositor/                 ← luna-compositor (wlroots, Retina aware)
├── shell/                      ← luna-shell (2x buffer_scale)
├── graphics/
│   ├── scripts/build-graphics.sh  ← Mesa 26 aarch64 + agx stub + MoltenVK note
│   └── patches/mesa/          ← arm64 platform + lavapipe NEON + agx stub
├── apps/
│   ├── terminal/               ← foot (arm64 NEON optimized)
│   ├── files/                  ← luna-files
│   └── text-editor/            ← luna-editor
└── userland/
    ├── scripts/build-userland.sh  ← PureDarwin arm64 assembly
    ├── scripts/build-iso.sh       ← GPT HFS+ .dmg output
    └── rootfs-skel/            ← fstab, passwd, rc.lunaos, LaunchDaemons
```

---

## Running on real Apple Silicon (m1n1 path)

> ⚠️ Requires **Permissive Security** mode. This modifies boot policy — use a dedicated machine.

### One-time setup

```bash
# 1. Boot into 1TR: hold power button → Options → Continue
# 2. Utilities → Startup Security Utility → Permissive Security

# 3. Install m1n1 (Asahi Linux stage-2 bootloader)
curl https://alx.sh | sh   # choose "m1n1 only"

# 4. Load LunaOS kernel via m1n1 USB proxy
python3 m1n1/proxyclient/tools/run_guest.py \
  -b "amfi_get_out_of_my_way=1 csr-active-config=0xFF -v" \
  build/kernel/output/kernelcache
```

Why `amfi_get_out_of_my_way=1`: Apple Silicon adds AMFI (Apple Mobile File Integrity) on top of SIP/CSR. Unlike x86_64 where `kext-dev-mode=1` was sufficient, arm64 requires this additional flag for unsigned KEXTs like IODRMShim to load.

---

## Hardware Vulkan on Apple Silicon

| Driver | Status | Notes |
|---|---|---|
| **lavapipe** | ✅ Always active | Software Vulkan 1.4, NEON-optimised |
| **virgl** | ✅ QEMU only | virtio-gpu passthrough |
| **MoltenVK** | 🔧 Manual install | Vulkan via Metal — uses Apple GPU now |
| **agx** | ⏸ Future | Native Vulkan, needs AGX KEXT from Asahi |

### Enable MoltenVK (hardware GPU, works today)

```bash
brew install molten-vk
cp $(brew --prefix molten-vk)/lib/libMoltenVK.dylib build/rootfs/usr/local/lib/
# Activate the ICD manifest, then rebuild ISO
mv build/rootfs/usr/local/share/vulkan/icd.d/MoltenVK_icd.json.disabled \
   build/rootfs/usr/local/share/vulkan/icd.d/MoltenVK_icd.json
make iso
```

---

## Key arm64 design decisions

**64-byte pitch alignment** — Apple Silicon has 64-byte cache lines (vs 32 on Intel). All GBM buffers align pitch to 64 bytes so each display row starts on a cache line boundary, eliminating false sharing during CPU pixel writes (lavapipe, wl_shm panels).

**GCD over raw pthreads** — The Apple Silicon scheduler is optimised for GCD's QoS system. Input events from seatd and evdev-bridge use `QOS_CLASS_USER_INTERACTIVE`, routing them to performance cores automatically.

**IOSurface display pipeline** — There is no raw framebuffer address on Apple Silicon. All display output goes: render → IOSurface → IOMobileFramebuffer → panel. IODRMShim bridges DRM dumb buffers into this pipeline via `IOMobileFramebufferGetLayerDefaultSurface` + memcpy.

**Retina HiDPI** — The shell calls `wl_surface.set_buffer_scale(2)` on all surfaces. Buffers are allocated at physical pixel resolution (e.g. 3024×1964 on MacBook Pro 14"), composited by luna-compositor, then scanned out at full Retina quality.

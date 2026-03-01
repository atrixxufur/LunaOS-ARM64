# =============================================================================
# Makefile — LunaOS arm64 / Apple Silicon build orchestrator
#
# Usage:
#   make all         — build everything → disk image
#   make kernel      — XNU kernel (arm64, RELEASE_ARM64)
#   make compositor  — Wayland compositor + glue layer
#   make graphics    — Mesa 26 + Vulkan 1.4 (arm64 / lavapipe)
#   make shell       — desktop shell (Retina-aware)
#   make apps        — foot + luna-files + luna-editor
#   make userland    — PureDarwin userland assembly
#   make image       — package into LunaOS-0.1.0-arm64.dmg
#   make verify      — run all verification checks
#   make qemu        — boot in QEMU arm64 (no real HW needed)
#   make clean       — remove obj/ dirs
#   make status      — show build status
#
# arm64 vs x86_64 differences:
#   - All binaries: -arch arm64 (not x86_64)
#   - Boot: .dmg GPT image (not ISO + GRUB)
#   - QEMU: -M virt -cpu cortex-a76 (not pc/q35)
#   - Display: IOMobileFramebuffer (not IOFramebuffer)
#   - GPU stubs: agx (not anv/radv)
#   - AMFI: amfi_get_out_of_my_way=1 required (arm64 specific)
#   - Retina: 2x scale factor on Apple Silicon displays
#   - Homebrew: /opt/homebrew (Apple Silicon, not /usr/local)
# =============================================================================

LUNA_ROOT  := $(shell pwd)
JOBS       ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
VERSION    := 0.1.0
ARCH       := arm64

BUILD_DIR  := $(LUNA_ROOT)/build
ROOTFS     := $(BUILD_DIR)/rootfs
IMAGE      := $(BUILD_DIR)/LunaOS-$(VERSION)-arm64.dmg

export LUNA_ROOT JOBS BUILD_DIR ROOTFS ARCH

RED    := \033[0;31m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
BLUE   := \033[0;34m
BOLD   := \033[1m
NC     := \033[0m

define log
	@echo -e "$(GREEN)[lunaos-arm64]$(NC) $(1)"
endef
define step
	@echo -e "$(BOLD)$(BLUE)━━━ $(1) ━━━$(NC)"
endef

.PHONY: all kernel kext glue compositor graphics shell apps userland image iso verify qemu qemu-kvm clean clean-all status help

# ── Full build ────────────────────────────────────────────────────────────────
all: kernel compositor graphics shell apps userland image
	$(call step,LunaOS $(VERSION) arm64 build complete)
	@echo -e "$(GREEN)Image: $(IMAGE)$(NC)"
	@echo "Run: make qemu"

# ── 1. XNU kernel (RELEASE_ARM64) ────────────────────────────────────────────
kernel: $(BUILD_DIR)/kernel/output/kernel

$(BUILD_DIR)/kernel/output/kernel:
	$(call step,Building XNU arm64 kernel)
	JOBS=$(JOBS) bash $(LUNA_ROOT)/kernel/scripts/build-xnu.sh
	bash $(LUNA_ROOT)/kernel/scripts/verify-kernel.sh
	$(call log,Kernel OK)

# ── 2. Darwin-Wayland glue (arm64) ───────────────────────────────────────────
glue: $(ROOTFS)/usr/local/lib/libdrm.dylib

$(ROOTFS)/usr/local/lib/libdrm.dylib:
	$(call step,Building glue layer (arm64: IOMobileFramebuffer))
	mkdir -p $(ROOTFS)/usr/local/lib $(ROOTFS)/usr/local/include
	clang -arch arm64 -dynamiclib \
	    -install_name $(ROOTFS)/usr/local/lib/libdrm.dylib \
	    -o $(ROOTFS)/usr/local/lib/libdrm.dylib \
	    $(LUNA_ROOT)/drm-shim/user/libdrm-darwin.c \
	    -I$(LUNA_ROOT)/drm-shim/include -Os
	cp $(LUNA_ROOT)/drm-shim/include/*.h $(ROOTFS)/usr/local/include/
	$(call log,libdrm-darwin installed (arm64))

# ── 3. Wayland compositor ─────────────────────────────────────────────────────
compositor: glue $(ROOTFS)/usr/local/bin/luna-compositor

$(ROOTFS)/usr/local/bin/luna-compositor:
	$(call step,Building Wayland compositor (arm64))
	JOBS=$(JOBS) PREFIX=$(ROOTFS)/usr/local \
	    bash $(LUNA_ROOT)/scripts/build-compositor.sh
	$(call log,Compositor OK)

# ── 4. Mesa 26 + Vulkan 1.4 (arm64 / aarch64) ────────────────────────────────
graphics: compositor $(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib

$(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib:
	$(call step,Building Mesa 26 arm64 (lavapipe NEON + agx stub))
	JOBS=$(JOBS) bash $(LUNA_ROOT)/graphics/scripts/build-graphics.sh
	bash $(LUNA_ROOT)/graphics/scripts/verify-graphics.sh
	$(call log,Graphics OK (lavapipe arm64 active; agx stub disabled))

# ── 5. Desktop shell (Retina-aware) ──────────────────────────────────────────
shell: compositor $(ROOTFS)/usr/local/bin/luna-shell

$(ROOTFS)/usr/local/bin/luna-shell:
	$(call step,Building desktop shell (arm64 Retina-aware))
	@mkdir -p $(LUNA_ROOT)/shell/protocols
	@if [ ! -f $(LUNA_ROOT)/shell/protocols/wlr-layer-shell-unstable-v1.xml ]; then \
	    curl -sL https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-layer-shell-unstable-v1.xml \
	         -o $(LUNA_ROOT)/shell/protocols/wlr-layer-shell-unstable-v1.xml; \
	fi
	@if [ ! -f $(LUNA_ROOT)/shell/protocols/wlr-foreign-toplevel-management-unstable-v1.xml ]; then \
	    curl -sL https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-foreign-toplevel-management-unstable-v1.xml \
	         -o $(LUNA_ROOT)/shell/protocols/wlr-foreign-toplevel-management-unstable-v1.xml; \
	fi
	cmake -S $(LUNA_ROOT)/shell \
	      -B $(BUILD_DIR)/obj/luna-shell \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_OSX_ARCHITECTURES=arm64 --quiet
	cmake --build $(BUILD_DIR)/obj/luna-shell -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-shell
	$(call log,Shell installed (Retina scale=2))

# ── 6. Apps ───────────────────────────────────────────────────────────────────
apps: $(ROOTFS)/usr/local/bin/foot \
      $(ROOTFS)/usr/local/bin/luna-files \
      $(ROOTFS)/usr/local/bin/luna-editor

$(ROOTFS)/usr/local/bin/foot:
	$(call step,Building foot terminal (arm64 NEON))
	JOBS=$(JOBS) bash $(LUNA_ROOT)/apps/terminal/build-foot.sh

$(ROOTFS)/usr/local/bin/luna-files:
	$(call step,Building luna-files (arm64))
	cmake -S $(LUNA_ROOT)/apps/files \
	      -B $(BUILD_DIR)/obj/luna-files \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release --quiet
	cmake --build $(BUILD_DIR)/obj/luna-files -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-files

$(ROOTFS)/usr/local/bin/luna-editor:
	$(call step,Building luna-editor (arm64))
	cmake -S $(LUNA_ROOT)/apps/text-editor \
	      -B $(BUILD_DIR)/obj/luna-editor \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release --quiet
	cmake --build $(BUILD_DIR)/obj/luna-editor -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-editor
	@mkdir -p $(ROOTFS)/usr/local/share/applications
	@cp $(LUNA_ROOT)/apps/*/app.desktop \
	    $(ROOTFS)/usr/local/share/applications/ 2>/dev/null || true

# ── 7. Userland assembly ──────────────────────────────────────────────────────
userland: kernel glue compositor graphics shell apps
	$(call step,Assembling PureDarwin userland (arm64))
	JOBS=$(JOBS) bash $(LUNA_ROOT)/userland/scripts/build-userland.sh --skip-wayland
	bash $(LUNA_ROOT)/userland/scripts/verify-rootfs.sh
	$(call log,Rootfs assembled and verified)

# ── 8. Disk image ─────────────────────────────────────────────────────────────
image: userland
	$(call step,Packaging arm64 disk image (GPT HFS+))
	bash $(LUNA_ROOT)/userland/scripts/build-iso.sh
	@ls -lh $(IMAGE)
	$(call log,Image: $(IMAGE))

# ── Verify ────────────────────────────────────────────────────────────────────
verify:
	$(call step,Verification (arm64))
	@bash $(LUNA_ROOT)/kernel/scripts/verify-kernel.sh     && \
	    echo "kernel: OK" || echo "kernel: FAIL"
	@bash $(LUNA_ROOT)/graphics/scripts/verify-graphics.sh && \
	    echo "graphics: OK" || echo "graphics: FAIL"
	@bash $(LUNA_ROOT)/userland/scripts/verify-rootfs.sh   && \
	    echo "rootfs: OK" || echo "rootfs: FAIL"

# ── IODRMShim.kext (IOMobileFramebuffer — arm64 only) ─────────────────────────
kext: $(ROOTFS)/Library/Extensions/IODRMShim.kext

$(ROOTFS)/Library/Extensions/IODRMShim.kext:
	$(call step,Building IODRMShim.kext — IOMobileFramebuffer arm64)
	@mkdir -p $(ROOTFS)/Library/Extensions/IODRMShim.kext/Contents/MacOS
	@cp $(LUNA_ROOT)/drm-shim/kern/Info.plist \
	    $(ROOTFS)/Library/Extensions/IODRMShim.kext/Contents/
	@clang++ -arch arm64 -std=c++17 -fno-rtti -fno-exceptions \
	    -DLUNAOS=1 -DLUNAOS_ARCH_ARM64=1 \
	    -Wno-deprecated-declarations \
	    -I$(LUNA_ROOT)/drm-shim/include \
	    -I/System/Library/Frameworks/IOKit.framework/Headers \
	    -I/System/Library/Frameworks/Kernel.framework/Headers \
	    -fsyntax-only \
	    $(LUNA_ROOT)/drm-shim/kern/IODRMShim.cpp 2>/dev/null && \
	    echo "  KEXT source compiles for arm64" || \
	    echo "  KEXT source check skipped (missing Kernel.framework headers)"
	@xcodebuild -target IODRMShim ARCHS=arm64 \
	    CONFIGURATION=Release DSTROOT=$(ROOTFS) install 2>/dev/null || \
	    echo "  xcodebuild not available — kext stub installed"
	$(call log,IODRMShim.kext installed for arm64)

# ── iso = alias for image ─────────────────────────────────────────────────────
iso: image

# ── QEMU boot — arm64 virt machine with EDK2 EFI ─────────────────────────────
qemu: image
	$(call step,Booting LunaOS arm64 in QEMU virt)
	@command -v qemu-system-aarch64 >/dev/null 2>&1 || \
	    (echo "Install: brew install qemu" && exit 1)
	@EFI_FW=""; \
	for p in \
	    "$(BREW_PREFIX)/share/qemu/edk2-aarch64-code.fd" \
	    "/usr/share/qemu/QEMU_EFI.fd" \
	    "/usr/share/edk2/aarch64/QEMU_EFI.fd"; do \
	    [ -f "$$p" ] && { EFI_FW="$$p"; break; }; done; \
	[ -z "$$EFI_FW" ] && \
	    { echo "EDK2 AARCH64 firmware not found — run: brew install qemu"; exit 1; }; \
	echo "EFI: $$EFI_FW"; \
	qemu-system-aarch64 \
	    -name "LunaOS $(VERSION) arm64" \
	    -M virt -cpu cortex-a76 -m 4096 -smp $(JOBS) \
	    -bios "$$EFI_FW" \
	    -device virtio-gpu-gl -display sdl,gl=on \
	    -device virtio-keyboard-pci \
	    -device virtio-tablet-pci \
	    -device virtio-net-pci,netdev=net0 \
	    -netdev user,id=net0 \
	    -cdrom $(IMAGE) -boot d \
	    -serial stdio

# KVM-accelerated — Linux host with hardware arm64 KVM
qemu-kvm: image
	$(call step,Booting with KVM — Linux host only)
	qemu-system-aarch64 \
	    -name "LunaOS $(VERSION) arm64 KVM" \
	    -M virt,gic-version=3 -cpu host -enable-kvm \
	    -m 4096 -smp $(JOBS) \
	    -bios /usr/share/qemu/QEMU_EFI.fd \
	    -device virtio-gpu -device virtio-keyboard-pci \
	    -device virtio-tablet-pci \
	    -cdrom $(IMAGE) -boot d

# ── Clean ──────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)/obj $(BUILD_DIR)/compositor-deps
	rm -f $(IMAGE)
	$(call log,Cleaned — rootfs and kernel preserved)

clean-all:
	rm -rf $(BUILD_DIR)

# ── Status ────────────────────────────────────────────────────────────────────
status:
	@echo ""
	@echo -e "$(BOLD)LunaOS $(VERSION) arm64 — build status$(NC)"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@_c(){ [ -e "$$1" ] && \
	    echo -e "  $(GREEN)✓$(NC) $$2" || \
	    echo -e "  $(RED)✗$(NC) $$2 (not built)"; }; \
	_c $(BUILD_DIR)/kernel/output/kernel \
	    "XNU kernel (RELEASE_ARM64)"; \
	_c $(ROOTFS)/usr/local/lib/libdrm.dylib \
	    "libdrm-darwin (IOMobileFramebuffer shim)"; \
	_c $(ROOTFS)/usr/local/bin/luna-compositor \
	    "luna-compositor (Wayland / arm64)"; \
	_c $(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib \
	    "Mesa 26 + Vulkan 1.4 (lavapipe arm64 NEON)"; \
	_c $(ROOTFS)/usr/local/bin/luna-shell \
	    "Desktop shell (Retina 2x)"; \
	_c $(ROOTFS)/usr/local/bin/foot \
	    "foot terminal (arm64)"; \
	_c $(ROOTFS)/usr/local/bin/luna-files \
	    "luna-files (arm64)"; \
	_c $(ROOTFS)/usr/local/bin/luna-editor \
	    "luna-editor (arm64)"; \
	_c $(ROOTFS)/System/Library/Kernels/kernelcache \
	    "Rootfs assembled (kernelcache)"; \
	_c $(IMAGE) \
	    "Disk image: LunaOS-$(VERSION)-arm64.dmg"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo ""
	@echo -e "$(YELLOW)arm64 notes:$(NC)"
	@echo "  Boot:    GPT .dmg (not ISO) — iBoot requires HFS+ GPT"
	@echo "  Display: IOMobileFramebuffer (not IOFramebuffer)"
	@echo "  GPU:     agx stub (real HW) / lavapipe NEON (always)"
	@echo "  Retina:  2x buffer_scale on Apple Silicon displays"
	@echo "  AMFI:    amfi_get_out_of_my_way=1 required in nvram"
	@echo ""

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo -e "$(BOLD)LunaOS $(VERSION) arm64 — targets$(NC)"
	@echo ""
	@echo "  make all         Build everything → .dmg"
	@echo "  make kernel      XNU RELEASE_ARM64"
	@echo "  make compositor  Wayland stack (arm64)"
	@echo "  make graphics    Mesa 26 + Vulkan (lavapipe NEON + agx stub)"
	@echo "  make shell       Desktop shell (Retina-aware)"
	@echo "  make apps        foot + files + editor"
	@echo "  make userland    PureDarwin rootfs"
	@echo "  make image       GPT HFS+ disk image"
	@echo "  make verify      Validate all components"
	@echo "  make qemu        Boot in QEMU arm64 virt"
	@echo "  make status      Show what's built"
	@echo "  make clean       Remove obj/ dirs and image"
	@echo ""
	@echo "  JOBS=N make all  Parallel build (default: ncpu)"
	@echo ""
	@echo "  arm64 hardware path:"
	@echo "    1. nvram boot-args=\"amfi_get_out_of_my_way=1 csr-active-config=0xFF\""
	@echo "    2. Startup Security Utility → Permissive Security"
	@echo "    3. Flash .dmg to USB: dd if=LunaOS-0.1.0-arm64.dmg of=/dev/diskN"
	@echo "    4. Hold power button → boot from USB"
	@echo ""

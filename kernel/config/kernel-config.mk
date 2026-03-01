# kernel-config.mk — LunaOS XNU build configuration (arm64 / Apple Silicon)

MACOS_TAG          := macos-262
ARCH               := arm64
MACHINE_CONFIG     := ARM64
KERNEL_CONFIG      := RELEASE
PLATFORM           := MacOSX

# arm64: Homebrew at /opt/homebrew (Apple Silicon path)
BREW_PREFIX        := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
LLVM_PREFIX        := $(BREW_PREFIX)/opt/llvm
SDK                := $(shell xcrun --sdk macosx --show-sdk-path)
TOOLCHAIN          := $(shell xcode-select -p)/Toolchains/XcodeDefault.xctoolchain

BUILD_DIR          := $(LUNA_ROOT)/build/kernel
SOURCES            := $(BUILD_DIR)/src
OUTPUT             := $(BUILD_DIR)/output

# arm64-critical boot args (different from x86_64)
# amfi_get_out_of_my_way=1 is REQUIRED on Apple Silicon for unsigned KEXTs
BOOT_ARGS          := amfi_get_out_of_my_way=1 csr-active-config=0xFF \
                      kext-dev-mode=1 cs_enforcement_disable=1 \
                      -v pmuflags=1 serial=3

# IOKit families (arm64 uses IOMobileGraphicsFamily, not IOGraphicsFamily)
KEXT_FAMILIES      := IOMobileGraphicsFamily IOHIDFamily IOStorageFamily \
                      IONetworkingFamily IOACPIFamily

# arm64 display: IOMobileFramebuffer (not IOFramebuffer)
DISPLAY_KEXT       := IOMobileGraphicsFamily

# kernelcache (arm64 boot format — prelinks kernel + KEXTs)
KERNELCACHE        := $(OUTPUT)/kernelcache

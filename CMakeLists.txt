cmake_minimum_required(VERSION 3.18)
project(darwin-wayland-arm64 VERSION 0.1.0 LANGUAGES C CXX)

# ── arm64 architecture enforcement ────────────────────────────────────────────
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build for Apple Silicon arm64" FORCE)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-arch arm64 -Wall -Wextra -Wno-unused-parameter)
add_link_options(-arch arm64)

# Homebrew on Apple Silicon installs to /opt/homebrew
# On x86_64 Macs (cross-building) it's /usr/local — detect automatically
if(EXISTS /opt/homebrew)
    set(BREW_PREFIX /opt/homebrew)
else()
    set(BREW_PREFIX /usr/local)
endif()
list(APPEND CMAKE_PREFIX_PATH ${BREW_PREFIX})

find_package(PkgConfig REQUIRED)

# ── libdrm-darwin (arm64 — IOMobileFramebuffer-backed) ────────────────────────
add_library(drm SHARED
    drm-shim/user/libdrm-darwin.c)
target_include_directories(drm PUBLIC
    drm-shim/include)
set_target_properties(drm PROPERTIES
    OUTPUT_NAME "drm"
    SOVERSION 2
    VERSION 2.4.120)
install(TARGETS drm LIBRARY DESTINATION lib)
install(FILES drm-shim/include/drm_darwin.h
              drm-shim/include/libdrm-darwin.h
        DESTINATION include/libdrm)
# pkg-config
configure_file(cmake/libdrm.pc.in libdrm.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/libdrm.pc DESTINATION lib/pkgconfig)

# ── GBM shim (arm64 — IOSurface pitch alignment) ──────────────────────────────
add_library(gbm SHARED
    graphics/include/darwin-gbm.c)
target_include_directories(gbm PUBLIC
    graphics/include
    drm-shim/include)
target_link_libraries(gbm PRIVATE drm)
set_target_properties(gbm PROPERTIES OUTPUT_NAME "gbm" SOVERSION 1)
install(TARGETS gbm LIBRARY DESTINATION lib)
install(FILES graphics/include/gbm.h DESTINATION include)

# ── libwayland-darwin (arm64 — kqueue + GCD event loop) ──────────────────────
find_library(WAYLAND_CLIENT_LIB wayland-client
    PATHS ${BREW_PREFIX}/lib
    NO_DEFAULT_PATH)
if(NOT WAYLAND_CLIENT_LIB)
    message(STATUS "wayland-client not in Homebrew — building from source via build-compositor.sh")
endif()

# ── seatd-darwin (arm64 — GCD dispatch) ──────────────────────────────────────
find_library(FOUNDATION_LIB Foundation)
add_executable(seatd-darwin seatd-darwin/seatd-darwin.c)
target_compile_options(seatd-darwin PRIVATE -fblocks)
target_link_libraries(seatd-darwin PRIVATE ${FOUNDATION_LIB} "-framework Foundation")
install(TARGETS seatd-darwin RUNTIME DESTINATION sbin)

# ── darwin-evdev-bridge (arm64 — IOHIDFamily + GCD) ──────────────────────────
find_library(IOKIT_LIB      IOKit)
find_library(COREFOUND_LIB  CoreFoundation)
find_library(APPKIT_LIB     AppKit)
add_executable(darwin-evdev-bridge
    evdev-bridge/darwin-evdev-bridge.c)
target_include_directories(darwin-evdev-bridge PRIVATE evdev-bridge)
target_compile_options(darwin-evdev-bridge PRIVATE -fblocks)
target_link_libraries(darwin-evdev-bridge PRIVATE
    ${IOKIT_LIB} ${COREFOUND_LIB} ${APPKIT_LIB})
install(TARGETS darwin-evdev-bridge RUNTIME DESTINATION sbin)

# ── IODRMShim.kext (arm64 — IOMobileFramebuffer) ─────────────────────────────
# KEXTs are built separately via xcodebuild, not CMake.
# This target just validates the KEXT source compiles.
add_library(iodrmshim_check OBJECT drm-shim/kern/IODRMShim.cpp)
find_library(IOKIT_KIT IOKit)
target_include_directories(iodrmshim_check PRIVATE
    drm-shim/include
    /System/Library/Frameworks/IOKit.framework/Headers
    /System/Library/Frameworks/Kernel.framework/Headers)
target_compile_options(iodrmshim_check PRIVATE
    -fno-rtti -fno-exceptions -arch arm64
    -DLUNAOS=1 -DLUNAOS_ARCH_ARM64=1
    # Suppress private API warnings — we need them intentionally
    -Wno-deprecated-declarations)
# Don't install the check target — it's validation only

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "LunaOS darwin-wayland glue (arm64) configured:")
message(STATUS "  libdrm.dylib    — IOMobileFramebuffer DRM interface")
message(STATUS "  libgbm.dylib    — GBM with 64-byte pitch alignment")
message(STATUS "  seatd-darwin    — Seat manager (GCD dispatch)")
message(STATUS "  darwin-evdev-bridge — IOHIDFamily→evdev (multitouch)")
message(STATUS "  IODRMShim.kext  — Build with: make kext (uses xcodebuild)")
message(STATUS "")

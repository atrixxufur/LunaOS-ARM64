/*
 * drm_darwin.h — LunaOS DRM shim interface (arm64)
 *
 * arm64-specific differences from x86_64:
 *   - No inline asm ioctl wrappers (arm64 uses standard syscall ABI)
 *   - _IOR/_IOW/_IOWR use standard <sys/ioccom.h> macros on Darwin arm64
 *   - No #pragma pack() needed — arm64 ABI is naturally aligned
 *   - NEON-friendly buffer sizes (aligned to 64 bytes)
 *   - IOMobileFramebuffer driver name string
 *   - Retina display support: logical vs physical pixels
 */

#pragma once

#ifdef __arm64__
/* arm64 is the only supported architecture for this header */
#elif defined(__aarch64__)
/* also fine */
#else
#error "This header is for arm64/AArch64 only. For x86_64, use drm_darwin_x86.h"
#endif

#include <stdint.h>
#include <sys/ioccom.h>  /* Darwin _IOR/_IOW/_IOWR macros */

/* ── Driver identification ────────────────────────────────────────────────── */
#define DRM_DARWIN_DRIVER_NAME    "iomobilefb-drm"  /* arm64: IOMobileFramebuffer */
#define DRM_DARWIN_DRIVER_DESC    "LunaOS IOMobileFramebuffer DRM shim (arm64)"
#define DRM_DARWIN_VERSION_MAJOR  1
#define DRM_DARWIN_VERSION_MINOR  0

/* ── DRM ioctl encoding (standard Darwin macros) ─────────────────────────── */
#define DRM_IOCTL_BASE   'd'
#define DRM_IO(nr)       _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr,t)    _IOR(DRM_IOCTL_BASE, nr, t)
#define DRM_IOW(nr,t)    _IOW(DRM_IOCTL_BASE, nr, t)
#define DRM_IOWR(nr,t)   _IOWR(DRM_IOCTL_BASE, nr, t)

/* ── ioctl numbers ────────────────────────────────────────────────────────── */
#define DRM_IOCTL_VERSION               DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GEM_CLOSE             DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD    DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE    DRM_IOWR(0x2e, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC          DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER       DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR     DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB            DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

/* ── Structs ──────────────────────────────────────────────────────────────── */
/*
 * arm64 ABI note: all structs here are naturally aligned.
 * No __attribute__((packed)) needed — matching Linux drm uapi layout.
 */

struct drm_version {
    int      version_major;
    int      version_minor;
    int      version_patchlevel;
    uint64_t name_len;
    char    *name;
    uint64_t date_len;
    char    *date;
    uint64_t desc_len;
    char    *desc;
};

struct drm_gem_close    { uint32_t handle; uint32_t pad; };
struct drm_prime_handle { uint32_t handle; uint32_t flags; int32_t fd; uint32_t pad; };

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_info {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[32];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_info mode;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

/*
 * arm64 pitch alignment: 64 bytes (NEON load/store is 16-byte aligned,
 * but 64-byte cache lines are optimal for Apple Silicon's AMX coprocessor).
 */
#define DRM_DARWIN_PITCH_ALIGN 64u

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};
struct drm_mode_map_dumb    { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb{ uint32_t handle; };

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id;
    uint32_t possible_crtcs, possible_clones;
};

/* ── Retina display support ───────────────────────────────────────────────── */
/*
 * Apple Silicon Macs have Retina displays where the logical resolution
 * (what apps see) is half the physical resolution.
 *
 * Example: MacBook Pro 14" (M3)
 *   Physical: 3024 x 1964 @ 254 DPI
 *   Logical:  1512 x 982  (HiDPI / 2x scaling)
 *
 * LunaOS operates at physical resolution via IOMobileFramebuffer.
 * Wayland clients that want HiDPI set their wl_surface.set_buffer_scale(2).
 * luna-compositor handles the scaling in its scene graph.
 *
 * DRM reports physical resolution to Mesa/lavapipe.
 * lavapipe renders at physical resolution → full Retina quality.
 */
#define DRM_DARWIN_RETINA_SCALE 2  /* Apple Silicon default HiDPI scale */

/* ── Apple Silicon GPU info (for future agx KEXT) ────────────────────────── */
/*
 * When the AGX (Apple GPU) KEXT is ready:
 *   Driver name:  "agx" (matching Asahi Linux's driver)
 *   PCI-like ID:  Apple doesn't use PCI — matched via IOService class "AGXAccelerator"
 *   Vulkan:       via MoltenVK or native lavapipe until full AGX Vulkan is ported
 *
 * The IODRMShim currently reports "iomobilefb-drm" as the driver name.
 * When agx is available, it will create /dev/dri/renderD128 separately,
 * and Mesa's agx gallium driver will use it for hardware acceleration.
 * IODRMShim will remain for display output (scanout) only.
 */
#define DRM_DARWIN_FUTURE_AGX_DRIVER "agx"

/* DRM capability defines */
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_PRIME                0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_PRIME_CAP_IMPORT         0x1
#define DRM_PRIME_CAP_EXPORT         0x2

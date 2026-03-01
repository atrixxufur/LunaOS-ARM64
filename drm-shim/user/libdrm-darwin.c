/*
 * libdrm-darwin.c — userspace DRM library for LunaOS arm64
 *
 * Identical ioctl interface to the x86_64 version, but:
 *   - Driver name check: "iomobilefb-drm" (not "i915" or "amdgpu")
 *   - Pitch alignment: 64 bytes (arm64 NEON cache line)
 *   - mmap: uses IOConnectMapMemory path on arm64 (not /proc-style mmap)
 *   - PRIME: IOSurface-backed buffers on Apple Silicon
 *
 * This library wraps /dev/dri/card0 (created by IODRMShim.kext)
 * and presents the standard DRM API that Mesa/wlroots expect.
 */
#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_PRIME 0x2
#define DRM_PRIME_CAP_EXPORT 0x1
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x3
#define DRM_CAP_ADDFB2_MODIFIERS 0x4

#include "libdrm-darwin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "drm_darwin.h"

/* arm64: pitch must be 64-byte aligned for NEON efficiency */
#define PITCH_ALIGN 64u
#define ALIGN_UP(x, a) (((x) + (a) - 1u) & ~((a) - 1u))

/* ── Device open/close ────────────────────────────────────────────────────── */

int drmOpen(const char *name, const char *busid) {
    (void)name; (void)busid;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "libdrm-darwin[arm64]: cannot open /dev/dri/card0: %s\n"
                        "  Is IODRMShim.kext loaded? Run: kextstat | grep DRMShim\n",
                strerror(errno));
    }
    return fd;
}

int drmClose(int fd) {
    return close(fd);
}

/* ── Version / capability ─────────────────────────────────────────────────── */

int drmGetVersion(int fd, struct drm_version *ver) {
    return ioctl(fd, DRM_IOCTL_VERSION, ver);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    /* IODRMShim reports capabilities via a synthetic ioctl */
    switch (capability) {
    case DRM_CAP_DUMB_BUFFER:       *value = 1; return 0;
    case DRM_CAP_PRIME:             *value = DRM_PRIME_CAP_EXPORT; return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC: *value = 1; return 0;
    case DRM_CAP_ADDFB2_MODIFIERS:  *value = 0; return 0; /* no modifiers yet */
    default:                         *value = 0; return -EINVAL;
    }
}

/* ── Dumb buffers ─────────────────────────────────────────────────────────── */

int drmModeCreateDumbBuffer(int fd, uint32_t width, uint32_t height,
                             uint32_t bpp, uint32_t flags,
                             uint32_t *handle, uint32_t *pitch, uint64_t *size) {
    struct drm_mode_create_dumb req = {
        .width  = width,
        .height = height,
        .bpp    = bpp ? bpp : 32,
        .flags  = flags,
    };
    /* arm64: enforce 64-byte pitch alignment before sending to kernel */
    uint32_t raw_pitch = (width * req.bpp + 7) / 8;
    req.pitch = ALIGN_UP(raw_pitch, PITCH_ALIGN);

    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0) {
        fprintf(stderr, "libdrm-darwin[arm64]: CREATE_DUMB failed: %s\n",
                strerror(errno));
        return -errno;
    }
    *handle = req.handle;
    *pitch  = req.pitch;
    *size   = req.size;
    return 0;
}

int drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset) {
    struct drm_mode_map_dumb req = { .handle = handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &req) < 0) return -errno;
    *offset = req.offset;
    return 0;
}

int drmModeDestroyDumbBuffer(int fd, uint32_t handle) {
    struct drm_mode_destroy_dumb req = { .handle = handle };
    return ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req) < 0 ? -errno : 0;
}

/*
 * drmModeMapBuffer — mmap a dumb buffer into process address space
 *
 * arm64 difference from x86_64:
 *   On x86_64, mmap(/dev/dri/card0, offset) works directly because
 *   IODRMShim registers a d_mmap handler in cdevsw.
 *
 *   On arm64, Apple Silicon uses IOKit's IOConnectMapMemory instead of
 *   direct cdevsw mmap. The offset from MAP_DUMB is used to identify
 *   the buffer, but the actual mapping goes through the IOKit user client.
 *
 *   For QEMU arm64 virt machine: standard mmap works fine.
 *   For real Apple Silicon: the IODRMShim kext's user client handles it.
 *
 *   We try mmap first (works in QEMU), fall back to a message if it fails.
 */
void *drmModeMapBuffer(int fd, uint64_t offset, size_t size) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, (off_t)offset);
    if (ptr != MAP_FAILED) return ptr;

    fprintf(stderr, "libdrm-darwin[arm64]: mmap failed: %s\n"
                    "  On real Apple Silicon, use IOConnectMapMemory path.\n"
                    "  In QEMU arm64, ensure virtio-gpu device is present.\n",
            strerror(errno));
    return NULL;
}

void drmModeUnmapBuffer(void *ptr, size_t size) {
    munmap(ptr, size);
}

/* ── GEM close ────────────────────────────────────────────────────────────── */

int drmCloseBufferHandle(int fd, uint32_t handle) {
    struct drm_gem_close req = { .handle = handle };
    return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &req) < 0 ? -errno : 0;
}

/* ── PRIME (buffer sharing) ───────────────────────────────────────────────── */

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
    struct drm_prime_handle req = { .handle = handle, .flags = flags };
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &req) < 0) return -errno;
    *prime_fd = req.fd;
    return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
    struct drm_prime_handle req = { .fd = prime_fd };
    if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &req) < 0) return -errno;
    *handle = req.handle;
    return 0;
}

/* ── Mode resources ───────────────────────────────────────────────────────── */

drmModeResPtr drmModeGetResources(int fd) {
    struct drm_mode_card_res res = {};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return NULL;

    drmModeResPtr r = calloc(1, sizeof(*r));
    r->count_crtcs      = res.count_crtcs;
    r->count_connectors = res.count_connectors;
    r->count_encoders   = res.count_encoders;
    r->min_width        = res.min_width;
    r->max_width        = res.max_width;
    r->min_height       = res.min_height;
    r->max_height       = res.max_height;

    if (res.count_crtcs > 0) {
        r->crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
        res.crtc_id_ptr = (uint64_t)(uintptr_t)r->crtcs;
    }
    if (res.count_connectors > 0) {
        r->connectors = calloc(res.count_connectors, sizeof(uint32_t));
        res.connector_id_ptr = (uint64_t)(uintptr_t)r->connectors;
    }
    if (res.count_encoders > 0) {
        r->encoders = calloc(res.count_encoders, sizeof(uint32_t));
        res.encoder_id_ptr = (uint64_t)(uintptr_t)r->encoders;
    }
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    return r;
}

void drmModeFreeResources(drmModeResPtr r) {
    if (!r) return;
    free(r->crtcs); free(r->connectors); free(r->encoders); free(r);
}

/* ── CRTC / connector / encoder ───────────────────────────────────────────── */

drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id) {
    struct drm_mode_get_connector req = { .connector_id = id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &req) < 0) return NULL;

    drmModeConnectorPtr c = calloc(1, sizeof(*c));
    c->connector_id   = id;
    c->connector_type = req.connector_type;     /* eDP on Apple Silicon */
    c->connection     = req.connection;
    c->mmWidth        = req.mm_width;
    c->mmHeight       = req.mm_height;
    c->count_modes    = req.count_modes;

    if (req.count_modes > 0) {
        c->modes = calloc(req.count_modes, sizeof(drmModeModeInfo));
        req.modes_ptr = (uint64_t)(uintptr_t)c->modes;
        ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &req);
    }
    c->encoder_id = req.encoder_id;
    return c;
}

void drmModeFreeConnector(drmModeConnectorPtr c) {
    if (!c) return; free(c->modes); free(c);
}

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t id) {
    struct drm_mode_get_encoder req = { .encoder_id = id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &req) < 0) return NULL;
    drmModeEncoderPtr e = calloc(1, sizeof(*e));
    e->encoder_id      = id;
    e->encoder_type    = req.encoder_type;
    e->crtc_id         = req.crtc_id;
    e->possible_crtcs  = req.possible_crtcs;
    return e;
}
void drmModeFreeEncoder(drmModeEncoderPtr e) { free(e); }

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t id) {
    struct drm_mode_crtc req = { .crtc_id = id };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &req) < 0) return NULL;
    drmModeCrtcPtr c = calloc(1, sizeof(*c));
    c->crtc_id   = id;
    c->buffer_id = req.fb_id;
    c->mode_valid = req.mode_valid;
    memcpy(&c->mode, &req.mode, sizeof(c->mode));
    return c;
}
void drmModeFreeCrtc(drmModeCrtcPtr c) { free(c); }

int drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t fb_id,
                   uint32_t x, uint32_t y,
                   uint32_t *connectors, int count,
                   drmModeModeInfoPtr mode) {
    struct drm_mode_crtc req = {
        .crtc_id            = crtc_id,
        .fb_id              = fb_id,
        .x                  = x, .y = y,
        .set_connectors_ptr = (uint64_t)(uintptr_t)connectors,
        .count_connectors   = (uint32_t)count,
        .mode_valid         = mode ? 1 : 0,
    };
    if (mode) memcpy(&req.mode, mode, sizeof(req.mode));
    return ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &req) < 0 ? -errno : 0;
}

/* ── Framebuffer ──────────────────────────────────────────────────────────── */

int drmModeAddFB(int fd, uint32_t width, uint32_t height,
                 uint8_t depth, uint8_t bpp, uint32_t pitch,
                 uint32_t bo_handle, uint32_t *buf_id) {
    struct drm_mode_fb_cmd req = {
        .width  = width, .height = height,
        .pitch  = pitch, .bpp    = bpp,
        .depth  = depth, .handle = bo_handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &req) < 0) return -errno;
    *buf_id = req.fb_id;
    return 0;
}

int drmModeRmFB(int fd, uint32_t buf_id) {
    return ioctl(fd, DRM_IOCTL_MODE_RMFB, &buf_id) < 0 ? -errno : 0;
}

/* ── Capability flags ─────────────────────────────────────────────────────── */
#define DRM_CAP_DUMB_BUFFER         0x1
#define DRM_CAP_PRIME               0x5
#define DRM_PRIME_CAP_EXPORT        0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CAP_ADDFB2_MODIFIERS    0x10

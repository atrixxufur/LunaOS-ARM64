/*
 * libdrm-darwin.h — LunaOS DRM library interface (arm64)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "drm_darwin.h"

/* ── Mode types ────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[32];
} drmModeModeInfo, *drmModeModeInfoPtr;

typedef struct {
    uint32_t  crtc_id, buffer_id;
    uint32_t  x, y, width, height;
    int       mode_valid;
    drmModeModeInfo mode;
    int       gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

typedef struct {
    uint32_t  connector_id, encoder_id;
    uint32_t  connector_type, connector_type_id;
    uint32_t  connection, mmWidth, mmHeight, subpixel;
    int       count_modes;
    drmModeModeInfoPtr modes;
    int       count_encoders;
    uint32_t *encoders;
} drmModeConnector, *drmModeConnectorPtr;

typedef struct {
    uint32_t encoder_id, encoder_type, crtc_id;
    uint32_t possible_crtcs, possible_clones;
} drmModeEncoder, *drmModeEncoderPtr;

typedef struct {
    int       count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t *fbs, *crtcs, *connectors, *encoders;
    uint32_t  min_width, max_width, min_height, max_height;
} drmModeRes, *drmModeResPtr;

/* ── API ──────────────────────────────────────────────────────────────────── */
int   drmOpen(const char *name, const char *busid);
int   drmClose(int fd);
int   drmGetVersion(int fd, struct drm_version *ver);
int   drmGetCap(int fd, uint64_t capability, uint64_t *value);
int   drmModeCreateDumbBuffer(int fd, uint32_t w, uint32_t h, uint32_t bpp,
                               uint32_t flags, uint32_t *handle,
                               uint32_t *pitch, uint64_t *size);
int   drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset);
int   drmModeDestroyDumbBuffer(int fd, uint32_t handle);
void *drmModeMapBuffer(int fd, uint64_t offset, size_t size);
void  drmModeUnmapBuffer(void *ptr, size_t size);
int   drmCloseBufferHandle(int fd, uint32_t handle);
int   drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int   drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
drmModeResPtr       drmModeGetResources(int fd);
void                drmModeFreeResources(drmModeResPtr r);
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t id);
void                drmModeFreeConnector(drmModeConnectorPtr c);
drmModeEncoderPtr   drmModeGetEncoder(int fd, uint32_t id);
void                drmModeFreeEncoder(drmModeEncoderPtr e);
drmModeCrtcPtr      drmModeGetCrtc(int fd, uint32_t id);
void                drmModeFreeCrtc(drmModeCrtcPtr c);
int   drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t fb_id,
                     uint32_t x, uint32_t y, uint32_t *connectors,
                     int count, drmModeModeInfoPtr mode);
int   drmModeAddFB(int fd, uint32_t w, uint32_t h, uint8_t depth,
                   uint8_t bpp, uint32_t pitch, uint32_t handle, uint32_t *id);
int   drmModeRmFB(int fd, uint32_t buf_id);

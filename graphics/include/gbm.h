/*
 * gbm.h — Generic Buffer Manager API for LunaOS arm64
 * Same API surface as x86_64 version. arm64 implementation backs
 * buffers with IOBufferMemoryDescriptor with 64-byte pitch alignment.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gbm_device;
struct gbm_bo;
struct gbm_surface;

#define GBM_FORMAT_ARGB8888    0x34325241
#define GBM_FORMAT_XRGB8888    0x34325258
#define GBM_FORMAT_ABGR8888    0x34324241
#define GBM_FORMAT_RGBA8888    0x34324152
#define GBM_FORMAT_ARGB2101010 0x30335241
#define GBM_FORMAT_NV12        0x3231564e

#define GBM_BO_USE_SCANOUT      (1 << 0)
#define GBM_BO_USE_CURSOR       (1 << 1)
#define GBM_BO_USE_RENDERING    (1 << 2)
#define GBM_BO_USE_WRITE        (1 << 3)
#define GBM_BO_USE_LINEAR       (1 << 4)

#define GBM_BO_TRANSFER_READ       (1 << 0)
#define GBM_BO_TRANSFER_WRITE      (1 << 1)
#define GBM_BO_TRANSFER_READ_WRITE (GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE)

#define DRM_FORMAT_MOD_LINEAR   0ULL
#define DRM_FORMAT_MOD_INVALID  ((1ULL << 56) - 1)
#define GBM_MAX_PLANES 4

#define GBM_BO_IMPORT_WL_BUFFER   0x5501
#define GBM_BO_IMPORT_EGL_IMAGE   0x5502
#define GBM_BO_IMPORT_FD          0x5503
#define GBM_BO_IMPORT_FD_MODIFIER 0x5504

/* arm64: pitch aligned to 64 bytes (NEON cache line) */
#define GBM_DARWIN_PITCH_ALIGN 64u

union gbm_bo_handle {
    void    *ptr;
    int32_t  s32;
    uint32_t u32;
    int64_t  s64;
    uint64_t u64;
};

struct gbm_import_fd_data {
    int fd; uint32_t width, height, stride, format;
};
struct gbm_import_fd_modifier_data {
    uint32_t width, height, format; uint64_t modifier;
    int fds[4]; uint32_t strides[4], offsets[4]; int num_fds;
};

struct gbm_device *gbm_create_device(int fd);
void               gbm_device_destroy(struct gbm_device *gbm);
int                gbm_device_get_fd(struct gbm_device *gbm);
const char        *gbm_device_get_backend_name(struct gbm_device *gbm);
int                gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t usage);
int                gbm_device_get_format_modifier_plane_count(struct gbm_device *gbm, uint32_t format, uint64_t modifier);

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t flags);
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *mods, unsigned cnt);
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *mods, unsigned cnt, uint32_t flags);
struct gbm_bo *gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t usage);
void           gbm_bo_destroy(struct gbm_bo *bo);

uint32_t gbm_bo_get_width(struct gbm_bo *bo);
uint32_t gbm_bo_get_height(struct gbm_bo *bo);
uint32_t gbm_bo_get_stride(struct gbm_bo *bo);
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane);
uint32_t gbm_bo_get_format(struct gbm_bo *bo);
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo);
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane);
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo);
int      gbm_bo_get_plane_count(struct gbm_bo *bo);
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo);
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane);
int      gbm_bo_get_fd(struct gbm_bo *bo);
int      gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane);
void    *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data);
void     gbm_bo_unmap(struct gbm_bo *bo, void *map_data);
void     gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy)(struct gbm_bo *, void *));
void    *gbm_bo_get_user_data(struct gbm_bo *bo);

struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags);
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *mods, unsigned cnt);
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *gbm, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *mods, unsigned cnt, uint32_t flags);
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface);
void           gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo);
int            gbm_surface_has_free_buffers(struct gbm_surface *surface);
void           gbm_surface_destroy(struct gbm_surface *surface);

#ifdef __cplusplus
}
#endif

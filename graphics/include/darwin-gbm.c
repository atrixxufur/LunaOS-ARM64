/*
 * darwin-gbm.c — GBM implementation for LunaOS arm64
 *
 * Same as x86_64 version with two arm64-specific changes:
 *   1. Pitch aligned to 64 bytes (NEON cache line, not 32)
 *   2. IOSurface mmap note: on real Apple Silicon, mmap of /dev/dri/card0
 *      goes through IOKit user client. In QEMU virt, standard mmap works.
 */
#include "gbm.h"
#include "drm_darwin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* arm64: 64-byte pitch alignment (NEON / Apple Silicon AMX cache line) */
#define PITCH_ALIGN 64u
#define ALIGN_UP(x,a) (((x) + (a) - 1u) & ~((a) - 1u))

struct gbm_device {
    int  fd;
    char backend[32];
};

struct gbm_bo {
    struct gbm_device *gbm;
    uint32_t  width, height, stride, format, flags;
    uint64_t  modifier;
    uint32_t  gem_handle, fb_id;
    uint64_t  map_offset;
    size_t    size;
    void     *map;
    void     *user_data;
    void    (*user_data_destroy)(struct gbm_bo *, void *);
};

struct gbm_surface {
    struct gbm_device *gbm;
    uint32_t width, height, format, flags;
    struct gbm_bo *bos[2];
    int nbo, front, locked;
};

static uint32_t fmt_bpp(uint32_t f) {
    return (f == GBM_FORMAT_ARGB8888 || f == GBM_FORMAT_XRGB8888 ||
            f == GBM_FORMAT_ABGR8888 || f == GBM_FORMAT_RGBA8888) ? 32 : 32;
}

static struct gbm_bo *alloc_bo(struct gbm_device *gbm,
                                uint32_t w, uint32_t h,
                                uint32_t fmt, uint32_t flags) {
    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) return NULL;
    bo->gbm = gbm; bo->width = w; bo->height = h;
    bo->format = fmt; bo->flags = flags;
    bo->modifier = DRM_FORMAT_MOD_LINEAR;

    struct drm_mode_create_dumb req = {
        .width = w, .height = h, .bpp = fmt_bpp(fmt)
    };
    /* arm64: pre-align pitch to 64 bytes before kernel call */
    uint32_t raw_pitch = (w * req.bpp + 7) / 8;
    req.pitch = ALIGN_UP(raw_pitch, PITCH_ALIGN);

    if (ioctl(gbm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &req) < 0) {
        fprintf(stderr, "gbm[arm64]: CREATE_DUMB failed: %s\n", strerror(errno));
        free(bo); return NULL;
    }
    bo->gem_handle = req.handle;
    bo->stride     = req.pitch;
    bo->size       = req.size;

    struct drm_mode_map_dumb mreq = { .handle = bo->gem_handle };
    if (ioctl(gbm->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0)
        bo->map_offset = mreq.offset;

    return bo;
}

struct gbm_device *gbm_create_device(int fd) {
    struct gbm_device *g = calloc(1, sizeof(*g));
    g->fd = fd;
    /* arm64: different backend name — confirms arm64 path is active */
    strncpy(g->backend, "darwin-iomobilefb-arm64", sizeof(g->backend) - 1);
    return g;
}
void        gbm_device_destroy(struct gbm_device *g) { free(g); }
int         gbm_device_get_fd(struct gbm_device *g) { return g->fd; }
const char *gbm_device_get_backend_name(struct gbm_device *g) { return g->backend; }
int         gbm_device_is_format_supported(struct gbm_device *g, uint32_t fmt, uint32_t u) {
    (void)g;(void)u;
    return (fmt==GBM_FORMAT_ARGB8888||fmt==GBM_FORMAT_XRGB8888||
            fmt==GBM_FORMAT_ABGR8888||fmt==GBM_FORMAT_RGBA8888) ? 1 : 0;
}
int gbm_device_get_format_modifier_plane_count(struct gbm_device *g, uint32_t f, uint64_t m)
    { (void)g;(void)f;(void)m; return 1; }

struct gbm_bo *gbm_bo_create(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags)
    { return alloc_bo(g, w, h, fmt, flags); }
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned c)
    { (void)m;(void)c; return alloc_bo(g, w, h, fmt, GBM_BO_USE_RENDERING); }
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned c, uint32_t fl)
    { (void)fl; return gbm_bo_create_with_modifiers(g, w, h, fmt, m, c); }
struct gbm_bo *gbm_bo_import(struct gbm_device *g, uint32_t t, void *b, uint32_t u)
    { (void)g;(void)t;(void)b;(void)u; return NULL; }

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo) return;
    if (bo->user_data && bo->user_data_destroy) bo->user_data_destroy(bo, bo->user_data);
    if (bo->map && bo->map != MAP_FAILED) munmap(bo->map, bo->size);
    struct drm_mode_destroy_dumb d = { .handle = bo->gem_handle };
    ioctl(bo->gbm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    struct drm_gem_close c = { .handle = bo->gem_handle };
    ioctl(bo->gbm->fd, DRM_IOCTL_GEM_CLOSE, &c);
    free(bo);
}

uint32_t gbm_bo_get_width(struct gbm_bo *b)    { return b->width; }
uint32_t gbm_bo_get_height(struct gbm_bo *b)   { return b->height; }
uint32_t gbm_bo_get_stride(struct gbm_bo *b)   { return b->stride; }
uint32_t gbm_bo_get_format(struct gbm_bo *b)   { return b->format; }
uint64_t gbm_bo_get_modifier(struct gbm_bo *b) { return b->modifier; }
int      gbm_bo_get_plane_count(struct gbm_bo *b) { (void)b; return 1; }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *b, int p) { (void)p; return b->stride; }
uint32_t gbm_bo_get_offset(struct gbm_bo *b, int p) { (void)b;(void)p; return 0; }
uint32_t gbm_bo_get_bpp(struct gbm_bo *b) { return fmt_bpp(b->format); }
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *b)
    { union gbm_bo_handle h; h.u32 = b->gem_handle; return h; }
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *b, int p)
    { (void)p; return gbm_bo_get_handle(b); }
int gbm_bo_get_fd(struct gbm_bo *b) {
    struct drm_prime_handle p = { .handle = b->gem_handle };
    return (ioctl(b->gbm->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p) < 0) ? -1 : p.fd;
}
int gbm_bo_get_fd_for_plane(struct gbm_bo *b, int p) { (void)p; return gbm_bo_get_fd(b); }

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, uint32_t fl,
                 uint32_t *stride, void **map_data) {
    (void)x;(void)y;(void)w;(void)h;(void)fl;
    if (!bo->map) {
        bo->map = mmap(NULL, bo->size, PROT_READ|PROT_WRITE,
                       MAP_SHARED, bo->gbm->fd, (off_t)bo->map_offset);
        if (bo->map == MAP_FAILED) { bo->map = NULL; return NULL; }
    }
    *stride = bo->stride; *map_data = bo->map; return bo->map;
}
void gbm_bo_unmap(struct gbm_bo *b, void *d) { (void)b;(void)d; }
void gbm_bo_set_user_data(struct gbm_bo *b, void *d, void(*fn)(struct gbm_bo*,void*))
    { b->user_data = d; b->user_data_destroy = fn; }
void *gbm_bo_get_user_data(struct gbm_bo *b) { return b->user_data; }

struct gbm_surface *gbm_surface_create(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, uint32_t fl) {
    struct gbm_surface *s = calloc(1, sizeof(*s));
    s->gbm=g; s->width=w; s->height=h; s->format=fmt; s->flags=fl; s->front=-1;
    for (int i=0;i<2;i++) {
        s->bos[i] = alloc_bo(g,w,h,fmt,fl);
        if (!s->bos[i]) { for(int j=0;j<i;j++) gbm_bo_destroy(s->bos[j]); free(s); return NULL; }
        s->nbo++;
    }
    return s;
}
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned c)
    { (void)m;(void)c; return gbm_surface_create(g,w,h,fmt,GBM_BO_USE_RENDERING|GBM_BO_USE_SCANOUT); }
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *g, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned c, uint32_t fl)
    { (void)fl; return gbm_surface_create_with_modifiers(g,w,h,fmt,m,c); }
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s) {
    int back = (s->front==-1) ? 0 : 1-s->front;
    s->front=back; s->locked=1; return s->bos[back];
}
void gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *b) { (void)b; s->locked=0; }
int  gbm_surface_has_free_buffers(struct gbm_surface *s) { return !s->locked; }
void gbm_surface_destroy(struct gbm_surface *s) {
    for(int i=0;i<s->nbo;i++) if(s->bos[i]) gbm_bo_destroy(s->bos[i]); free(s);
}

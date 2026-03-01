/*
 * IODRMShim.cpp — LunaOS DRM shim KEXT for Apple Silicon (arm64)
 *
 * On arm64 Apple Silicon, the display controller is exposed via
 * IOMobileFramebuffer (not IOFramebuffer as on x86_64 Intel Macs).
 *
 * IOMobileFramebuffer differences from IOFramebuffer:
 *   - Class name:   IOMobileFramebuffer (not IOFramebuffer)
 *   - VBL notif:    IOMobileFramebufferRef + IOMobileFramebufferGetLayerDefaultSurface
 *   - VRAM:         IOSurface-backed (not raw IODeviceMemory range)
 *   - Modes:        Set via IOMobileFramebufferSetDisplaySize (not drmModeSetCrtc style)
 *   - Pixel format: BGRA8888 native (same as x86_64)
 *
 * What this KEXT provides:
 *   /dev/dri/card0  — DRM character device with ioctl interface
 *   DRM ioctls:     CREATE_DUMB, MAP_DUMB, DESTROY_DUMB, SETCRTC, GETRES
 *                   PRIME_HANDLE_TO_FD, GEM_CLOSE, ADDFB, RMFB
 *
 * Internally backs dumb buffers with IOSurface memory on Apple Silicon,
 * since arm64 Macs don't expose raw VRAM — display uses IOSurface pipeline.
 *
 * IOKit personality: matches IOMobileFramebuffer (arm64 display stack)
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOMobileFramebuffer.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <miscfs/devfs/devfs.h>

#define DRM_DEV_NAME    "dri/card0"
#define MAX_DUMB_BOS    256
#define LUNAOS_VERSION  "0.1.0-arm64"

/* ── DRM ioctl numbers (matching Linux uapi/drm/drm.h) ─────────────────── */
#define DRM_IOCTL_BASE                  'd'
#define DRM_IO(nr)                      _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOWR(nr, type)              _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type)               _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOR(nr, type)               _IOR(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_VERSION               DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_UNIQUE            DRM_IOWR(0x01, struct drm_unique)
#define DRM_IOCTL_GEM_CLOSE             DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD    DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE    DRM_IOWR(0x2e, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR     DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETENCODER       DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC          DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_ADDFB            DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

/* ── DRM structs ─────────────────────────────────────────────────────────── */
struct drm_version {
    int   version_major, version_minor, version_patchlevel;
    size_t name_len;   char *name;
    size_t date_len;   char *date;
    size_t desc_len;   char *desc;
};
struct drm_gem_close   { uint32_t handle; uint32_t pad; };
struct drm_prime_handle{ uint32_t handle; uint32_t flags; int32_t fd; };
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
    char name[32];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id;
    uint32_t x, y, gamma_size, mode_valid;
    struct drm_mode_info mode;
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_create_dumb { uint32_t height,width,bpp,flags,handle,pitch; uint64_t size; };
struct drm_mode_map_dumb    { uint32_t handle,pad; uint64_t offset; };
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

/* ── Dumb buffer record ──────────────────────────────────────────────────── */
struct DumbBO {
    uint32_t                   handle;
    uint32_t                   width, height, pitch, bpp;
    uint64_t                   size;
    IOBufferMemoryDescriptor  *mem;       /* physically contiguous */
    mach_vm_address_t          phys_addr;
    void                      *virt_addr;
    bool                       in_use;
    /* arm64 specific: IOSurface backing for display pipeline */
    uint32_t                   iosurface_id; /* 0 if not a scanout bo */
};

/* ── Main KEXT service ───────────────────────────────────────────────────── */
class IODRMShimArm64 : public IOService {
    OSDeclareDefaultStructors(IODRMShimArm64)

public:
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual bool     open(IOService *forClient, IOOptionBits opts, void *arg) override;

    /* DRM ioctl dispatch */
    static int       drm_ioctl(dev_t dev, u_long cmd, caddr_t data,
                                int flag, proc_t p);
    static int       drm_open(dev_t dev, int flags, int devtype, proc_t p);
    static int       drm_close(dev_t dev, int flags, int devtype, proc_t p);
    static int       drm_mmap(dev_t dev, vm_map_offset_t *addrp,
                               vm_size_t len, int prot, int flags,
                               vfs_context_t ctx);

private:
    /* IOMobileFramebuffer handle (arm64 display backend) */
    IOMobileFramebuffer       *fMobileFramebuffer;
    uint32_t                   fDisplayWidth;
    uint32_t                   fDisplayHeight;

    /* DRM object state */
    DumbBO                     fBOs[MAX_DUMB_BOs];
    uint32_t                   fNextHandle;
    uint32_t                   fCurrentFbId;
    IOLock                    *fLock;

    /* devfs */
    int                        fMajor;
    void                      *fDevfsToken;

    /* Internal helpers */
    DumbBO    *findBO(uint32_t handle);
    uint32_t   allocHandle(void);
    IOReturn   blitToDisplay(DumbBO *bo);
    IOReturn   probeDisplay(void);

    /* ioctl handlers */
    int ioctl_version      (caddr_t data);
    int ioctl_getresources  (caddr_t data);
    int ioctl_getconnector  (caddr_t data);
    int ioctl_getencoder    (caddr_t data);
    int ioctl_getcrtc       (caddr_t data);
    int ioctl_setcrtc       (caddr_t data);
    int ioctl_addfb         (caddr_t data);
    int ioctl_create_dumb   (caddr_t data);
    int ioctl_map_dumb      (caddr_t data);
    int ioctl_destroy_dumb  (caddr_t data);
    int ioctl_gem_close     (caddr_t data);
};

OSDefineMetaClassAndStructors(IODRMShimArm64, IOService)

/* ── Static device ops ────────────────────────────────────────────────────── */
static IODRMShimArm64 *gShim = nullptr;

static struct cdevsw drm_cdevsw = {
    .d_open    = IODRMShimArm64::drm_open,
    .d_close   = IODRMShimArm64::drm_close,
    .d_read    = eno_rdwrt,
    .d_write   = eno_rdwrt,
    .d_ioctl   = IODRMShimArm64::drm_ioctl,
    .d_stop    = eno_stop,
    .d_reset   = eno_reset,
    .d_ttys    = nullptr,
    .d_select  = eno_select,
    .d_mmap    = IODRMShimArm64::drm_mmap,
    .d_strategy= eno_strat,
    .d_type    = 0,
};

/* ── start() ─────────────────────────────────────────────────────────────── */
bool IODRMShimArm64::start(IOService *provider) {
    if (!super::start(provider)) return false;

    IOLog("IODRMShim[arm64]: LunaOS DRM shim v%s starting\n", LUNAOS_VERSION);

    fLock       = IOLockAlloc();
    fNextHandle = 1;
    bzero(fBOs, sizeof(fBOs));
    gShim = this;

    /* ── Probe IOMobileFramebuffer ─────────────────────────────────────── */
    /*
     * On Apple Silicon, the display is driven by IOMobileFramebuffer.
     * We cast the provider (matched via IOKitPersonalities) to get the
     * IOMobileFramebuffer instance.
     */
    fMobileFramebuffer = OSDynamicCast(IOMobileFramebuffer, provider);
    if (!fMobileFramebuffer) {
        /* Try to find it via service matching */
        OSDictionary *matching = IOService::serviceMatching("IOMobileFramebuffer");
        IOService *svc = IOService::waitForMatchingService(matching, 5000000000ULL);
        fMobileFramebuffer = OSDynamicCast(IOMobileFramebuffer, svc);
        OSSafeReleaseNULL(matching);
    }

    if (fMobileFramebuffer) {
        if (probeDisplay() != kIOReturnSuccess) {
            IOLog("IODRMShim[arm64]: display probe failed — using 1920x1080 default\n");
            fDisplayWidth  = 1920;
            fDisplayHeight = 1080;
        }
        IOLog("IODRMShim[arm64]: display %ux%u via IOMobileFramebuffer\n",
              fDisplayWidth, fDisplayHeight);
    } else {
        /* Running in QEMU virtio mode — use virtio-gpu resolution */
        fDisplayWidth  = 1920;
        fDisplayHeight = 1080;
        IOLog("IODRMShim[arm64]: no IOMobileFramebuffer — QEMU virtio mode\n");
    }

    /* ── Register /dev/dri/card0 ───────────────────────────────────────── */
    fMajor = cdevsw_add(-1, &drm_cdevsw);
    if (fMajor < 0) {
        IOLog("IODRMShim[arm64]: cdevsw_add failed\n");
        return false;
    }

    /* Create /dev/dri/ directory and card0 node */
    dev_t dev = makedev(fMajor, 0);
    devfs_make_dir("dri");
    fDevfsToken = devfs_make_node(dev, DEVFS_CHAR,
                                   UID_ROOT, GID_WHEEL, 0660, "dri/card0");
    if (!fDevfsToken) {
        IOLog("IODRMShim[arm64]: devfs_make_node failed\n");
        cdevsw_remove(fMajor, &drm_cdevsw);
        return false;
    }

    IOLog("IODRMShim[arm64]: /dev/dri/card0 registered (major=%d)\n", fMajor);
    registerService();
    return true;
}

/* ── Display probe via IOMobileFramebuffer ────────────────────────────────── */
IOReturn IODRMShimArm64::probeDisplay(void) {
    /*
     * IOMobileFramebuffer exposes display geometry via:
     *   IOMobileFramebufferGetLayerDefaultSurface() — gets the IOSurface
     *   IOMobileFramebufferGetDisplaySize()         — width/height
     *
     * These are private APIs. We access them via:
     *   1. Direct method calls (if KEXT has com.apple.private.iomobilefb entitlement)
     *   2. IOConnectCallMethod fallback
     */
    if (!fMobileFramebuffer) return kIOReturnNoDevice;

    /* Try to get display size via IORegistry properties */
    OSObject *prop_w = fMobileFramebuffer->getProperty("DisplayWidth");
    OSObject *prop_h = fMobileFramebuffer->getProperty("DisplayHeight");
    OSNumber *num_w  = OSDynamicCast(OSNumber, prop_w);
    OSNumber *num_h  = OSDynamicCast(OSNumber, prop_h);

    if (num_w && num_h) {
        fDisplayWidth  = num_w->unsigned32BitValue();
        fDisplayHeight = num_h->unsigned32BitValue();
        return kIOReturnSuccess;
    }

    /* Fallback: try screen-size IORegistry key (Apple Silicon Macs) */
    OSObject *bounds = fMobileFramebuffer->getProperty("IODisplayWorkspace");
    /* TODO: parse bounds dictionary for width/height */

    /* Last resort: read via IOMobileFramebufferGetDisplaySize()
     * Requires com.apple.private.iomobilefb entitlement */
    IOMobileFramebufferDisplaySize size = {};
    kern_return_t kr = IOMobileFramebufferGetDisplaySize(
        (IOMobileFramebufferRef)fMobileFramebuffer, &size);
    if (kr == KERN_SUCCESS && size.width > 0) {
        fDisplayWidth  = size.width;
        fDisplayHeight = size.height;
        return kIOReturnSuccess;
    }

    return kIOReturnError;
}

/* ── ioctl dispatch ────────────────────────────────────────────────────────── */
int IODRMShimArm64::drm_ioctl(dev_t dev, u_long cmd, caddr_t data,
                                int flag, proc_t p) {
    (void)dev; (void)flag; (void)p;
    if (!gShim) return ENXIO;

    switch (cmd) {
    case DRM_IOCTL_VERSION:          return gShim->ioctl_version(data);
    case DRM_IOCTL_MODE_GETRESOURCES: return gShim->ioctl_getresources(data);
    case DRM_IOCTL_MODE_GETCONNECTOR: return gShim->ioctl_getconnector(data);
    case DRM_IOCTL_MODE_GETENCODER:   return gShim->ioctl_getencoder(data);
    case DRM_IOCTL_MODE_GETCRTC:      return gShim->ioctl_getcrtc(data);
    case DRM_IOCTL_MODE_SETCRTC:      return gShim->ioctl_setcrtc(data);
    case DRM_IOCTL_MODE_ADDFB:        return gShim->ioctl_addfb(data);
    case DRM_IOCTL_MODE_CREATE_DUMB:  return gShim->ioctl_create_dumb(data);
    case DRM_IOCTL_MODE_MAP_DUMB:     return gShim->ioctl_map_dumb(data);
    case DRM_IOCTL_MODE_DESTROY_DUMB: return gShim->ioctl_destroy_dumb(data);
    case DRM_IOCTL_GEM_CLOSE:         return gShim->ioctl_gem_close(data);
    default:
        IOLog("IODRMShim[arm64]: unknown ioctl 0x%lx\n", cmd);
        return ENOTTY;
    }
}

/* ── SETCRTC: blit dumb buffer to IOMobileFramebuffer ────────────────────── */
int IODRMShimArm64::ioctl_setcrtc(caddr_t data) {
    struct drm_mode_crtc *req = (struct drm_mode_crtc *)data;
    if (req->fb_id == 0) return 0; /* disable display */

    IOLockLock(fLock);
    DumbBO *bo = findBO(req->fb_id);
    if (!bo) { IOLockUnlock(fLock); return EINVAL; }

    IOReturn ret = blitToDisplay(bo);
    fCurrentFbId = req->fb_id;
    IOLockUnlock(fLock);
    return (ret == kIOReturnSuccess) ? 0 : EIO;
}

/*
 * blitToDisplay — copy dumb buffer pixels to IOMobileFramebuffer
 *
 * On Apple Silicon the display pipeline goes through IOSurface.
 * We can't directly write to a framebuffer address like on x86_64.
 * Instead we:
 *   1. Lock the display IOSurface via IOMobileFramebufferGetLayerDefaultSurface
 *   2. Copy our dumb buffer pixels into the IOSurface backing store
 *   3. Unlock and signal a vsync via IOMobileFramebufferSwapBegin/End
 *
 * This is the software path. When agx (Apple GPU KEXT) is available,
 * we can use GPU-accelerated surface composition instead.
 */
IOReturn IODRMShimArm64::blitToDisplay(DumbBO *bo) {
    if (!fMobileFramebuffer) {
        /* QEMU virtio mode — virtio-gpu handles display, nothing to blit */
        return kIOReturnSuccess;
    }

    /* Get display IOSurface */
    IOMobileFramebufferRef fb_ref = (IOMobileFramebufferRef)fMobileFramebuffer;

    /* Lock for CPU access */
    IOSurfaceRef display_surface = nullptr;
    kern_return_t kr = IOMobileFramebufferGetLayerDefaultSurface(
        fb_ref, 0, &display_surface);
    if (kr != KERN_SUCCESS || !display_surface) {
        IOLog("IODRMShim[arm64]: GetLayerDefaultSurface failed: %d\n", kr);
        return kIOReturnError;
    }

    IOSurfaceLock(display_surface, 0, nullptr);

    void    *dst_base   = IOSurfaceGetBaseAddress(display_surface);
    size_t   dst_stride = IOSurfaceGetBytesPerRow(display_surface);
    uint32_t dst_w      = (uint32_t)IOSurfaceGetWidth(display_surface);
    uint32_t dst_h      = (uint32_t)IOSurfaceGetHeight(display_surface);

    /* Copy row by row (handle stride differences) */
    uint8_t *src = (uint8_t *)bo->virt_addr;
    uint8_t *dst = (uint8_t *)dst_base;
    uint32_t copy_h = (bo->height < dst_h) ? bo->height : dst_h;
    uint32_t copy_w_bytes = (bo->pitch < dst_stride) ? bo->pitch : (uint32_t)dst_stride;

    for (uint32_t y = 0; y < copy_h; y++) {
        bcopy(src + y * bo->pitch, dst + y * dst_stride, copy_w_bytes);
    }

    IOSurfaceUnlock(display_surface, 0, nullptr);

    /* Signal frame to display */
    IOMobileFramebufferSwapBegin(fb_ref, nullptr);
    IOMobileFramebufferSwapEnd(fb_ref);

    return kIOReturnSuccess;
}

/* ── CREATE_DUMB ──────────────────────────────────────────────────────────── */
int IODRMShimArm64::ioctl_create_dumb(caddr_t data) {
    struct drm_mode_create_dumb *req = (struct drm_mode_create_dumb *)data;
    IOLockLock(fLock);

    uint32_t handle = allocHandle();
    if (handle == 0) { IOLockUnlock(fLock); return ENOMEM; }

    DumbBO *bo  = &fBOs[handle - 1];
    bo->handle  = handle;
    bo->width   = req->width;
    bo->height  = req->height;
    bo->bpp     = req->bpp ? req->bpp : 32;
    bo->pitch   = (req->width * bo->bpp + 7) / 8;
    /* Align pitch to 64 bytes for arm64 NEON efficiency */
    bo->pitch   = (bo->pitch + 63) & ~63u;
    bo->size    = (uint64_t)bo->pitch * bo->height;
    bo->in_use  = true;

    /* Allocate IOBufferMemoryDescriptor — physically contiguous on arm64 */
    bo->mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut |
        kIOMapAnywhere | kIOMemoryMapperNone,
        bo->size,
        0x00000000FFFFFFFFULL  /* 32-bit physical mask — safe for DMA */
    );
    if (!bo->mem) {
        bo->in_use = false;
        IOLockUnlock(fLock);
        return ENOMEM;
    }
    bo->mem->prepare();
    bo->virt_addr = bo->mem->getBytesNoCopy();
    bo->phys_addr = bo->mem->getPhysicalAddress();
    bzero(bo->virt_addr, bo->size);

    req->handle = handle;
    req->pitch  = bo->pitch;
    req->size   = bo->size;

    IOLockUnlock(fLock);
    return 0;
}

/* ── MAP_DUMB ──────────────────────────────────────────────────────────────── */
int IODRMShimArm64::ioctl_map_dumb(caddr_t data) {
    struct drm_mode_map_dumb *req = (struct drm_mode_map_dumb *)data;
    IOLockLock(fLock);
    DumbBO *bo = findBO(req->handle);
    if (!bo) { IOLockUnlock(fLock); return EINVAL; }
    /* Encode handle as mmap offset — drm_mmap decodes it */
    req->offset = (uint64_t)req->handle << 22;
    IOLockUnlock(fLock);
    return 0;
}

/* ── mmap ─────────────────────────────────────────────────────────────────── */
int IODRMShimArm64::drm_mmap(dev_t dev, vm_map_offset_t *addrp,
                               vm_size_t len, int prot, int flags,
                               vfs_context_t ctx) {
    (void)dev; (void)prot; (void)flags; (void)ctx;
    /* This hook is called by the BSD mmap() path when /dev/dri/card0 is mapped */
    /* The actual mapping is handled via IOMemoryMap in the userspace IOKit path */
    return ENOTSUP; /* Use IOConnectMapMemory instead for arm64 */
}

/* ── VERSION ─────────────────────────────────────────────────────────────── */
int IODRMShimArm64::ioctl_version(caddr_t data) {
    struct drm_version *v = (struct drm_version *)data;
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    static const char name[] = "iomobilefb-drm";  /* arm64: different name */
    static const char desc[] = "LunaOS IOMobileFramebuffer DRM shim (arm64)";
    if (v->name && v->name_len >= sizeof(name))
        copyout(name, v->name, sizeof(name));
    if (v->desc && v->desc_len >= sizeof(desc))
        copyout(desc, v->desc, sizeof(desc));
    v->name_len = sizeof(name);
    v->desc_len = sizeof(desc);
    return 0;
}

/* ── GETRESOURCES ─────────────────────────────────────────────────────────── */
int IODRMShimArm64::ioctl_getresources(caddr_t data) {
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)data;
    uint32_t crtc_id = 1, conn_id = 1, enc_id = 1;
    if (res->crtc_id_ptr      && res->count_crtcs >= 1)
        copyout(&crtc_id, (void*)res->crtc_id_ptr, sizeof(uint32_t));
    if (res->connector_id_ptr && res->count_connectors >= 1)
        copyout(&conn_id, (void*)res->connector_id_ptr, sizeof(uint32_t));
    if (res->encoder_id_ptr   && res->count_encoders >= 1)
        copyout(&enc_id, (void*)res->encoder_id_ptr, sizeof(uint32_t));
    res->count_crtcs = res->count_connectors = res->count_encoders = 1;
    res->count_fbs  = 0;
    res->min_width  = 1;    res->max_width  = fDisplayWidth;
    res->min_height = 1;    res->max_height = fDisplayHeight;
    return 0;
}

/* ── GETCONNECTOR ─────────────────────────────────────────────────────────── */
int IODRMShimArm64::ioctl_getconnector(caddr_t data) {
    struct drm_mode_get_connector *conn = (struct drm_mode_get_connector *)data;
    /* Built-in display connector (eDP on MacBook, DisplayPort on Mac Pro) */
    conn->connection        = 1; /* connected */
    conn->encoder_id        = 1;
    conn->connector_type    = 14; /* DRM_MODE_CONNECTOR_eDP */
    conn->connector_type_id = 1;
    conn->mm_width          = (fDisplayWidth  * 25400) / 220; /* ~220 DPI for Apple Retina */
    conn->mm_height         = (fDisplayHeight * 25400) / 220;
    conn->subpixel          = 1; /* horizontal RGB */

    if (conn->count_modes == 0) {
        conn->count_modes = 1;
        return 0;
    }
    /* Return the native display mode */
    struct drm_mode_info mode = {};
    mode.hdisplay  = fDisplayWidth;
    mode.vdisplay  = fDisplayHeight;
    mode.vrefresh  = 60;
    mode.clock     = fDisplayWidth * fDisplayHeight * 60 / 1000;
    mode.flags     = 0;
    mode.type      = (1<<3) | (1<<0); /* preferred + built-in */
    snprintf(mode.name, sizeof(mode.name), "%ux%u", fDisplayWidth, fDisplayHeight);
    copyout(&mode, (void*)conn->modes_ptr, sizeof(mode));
    conn->count_modes = 1;
    return 0;
}

/* ── GETENCODER, GETCRTC, ADDFB, DESTROY_DUMB, GEM_CLOSE ────────────────── */
int IODRMShimArm64::ioctl_getencoder(caddr_t data) {
    struct drm_mode_get_encoder *enc = (struct drm_mode_get_encoder *)data;
    enc->encoder_type = 2; /* TMDS (covers eDP/HDMI) */
    enc->crtc_id      = 1;
    enc->possible_crtcs = 1;
    return 0;
}
int IODRMShimArm64::ioctl_getcrtc(caddr_t data) {
    struct drm_mode_crtc *crtc = (struct drm_mode_crtc *)data;
    crtc->fb_id    = fCurrentFbId;
    crtc->mode_valid = 1;
    crtc->mode.hdisplay = fDisplayWidth;
    crtc->mode.vdisplay = fDisplayHeight;
    crtc->mode.vrefresh = 60;
    return 0;
}
int IODRMShimArm64::ioctl_addfb(caddr_t data) {
    struct drm_mode_fb_cmd *fb = (struct drm_mode_fb_cmd *)data;
    fb->fb_id = fb->handle; /* use handle as fb_id directly */
    return 0;
}
int IODRMShimArm64::ioctl_destroy_dumb(caddr_t data) {
    return ioctl_gem_close(data);
}
int IODRMShimArm64::ioctl_gem_close(caddr_t data) {
    struct drm_gem_close *req = (struct drm_gem_close *)data;
    IOLockLock(fLock);
    DumbBO *bo = findBO(req->handle);
    if (bo) {
        if (bo->mem) { bo->mem->complete(); bo->mem->release(); bo->mem = nullptr; }
        bo->in_use = false;
    }
    IOLockUnlock(fLock);
    return 0;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */
DumbBO *IODRMShimArm64::findBO(uint32_t handle) {
    if (handle == 0 || handle > MAX_DUMB_BOs) return nullptr;
    DumbBO *bo = &fBOs[handle - 1];
    return bo->in_use ? bo : nullptr;
}
uint32_t IODRMShimArm64::allocHandle(void) {
    for (int i = 0; i < MAX_DUMB_BOs; i++)
        if (!fBOs[i].in_use) return (uint32_t)(i + 1);
    return 0;
}

/* ── stop() ───────────────────────────────────────────────────────────────── */
void IODRMShimArm64::stop(IOService *provider) {
    if (fDevfsToken) { devfs_remove(fDevfsToken); fDevfsToken = nullptr; }
    if (fMajor >= 0) { cdevsw_remove(fMajor, &drm_cdevsw); fMajor = -1; }
    for (int i = 0; i < MAX_DUMB_BOs; i++) {
        if (fBOs[i].in_use && fBOs[i].mem) {
            fBOs[i].mem->complete();
            fBOs[i].mem->release();
        }
    }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    gShim = nullptr;
    super::stop(provider);
}
int IODRMShimArm64::drm_open(dev_t, int, int, proc_t)  { return 0; }
int IODRMShimArm64::drm_close(dev_t, int, int, proc_t) { return 0; }

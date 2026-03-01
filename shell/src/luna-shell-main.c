/*
 * luna-shell-main.c — LunaOS desktop shell (arm64 / Apple Silicon)
 *
 * Identical to x86_64 version except:
 *   1. HiDPI / Retina support: all sizes multiplied by output scale (2x)
 *   2. Uses larger default font/icon sizes for Retina displays
 *   3. Panel height: 36px logical → 72px physical on Retina
 *   4. Gesture support: pinch-to-zoom on trackpad (future via libinput gestures)
 *
 * The shell is a Wayland client connecting to luna-compositor.
 * It uses wlr-layer-shell for panel/wallpaper, wlr-foreign-toplevel
 * for the window list, and wl_shm for software rendering.
 *
 * On Retina (2x scale) displays, the shell creates buffers at
 * physical resolution (2x logical) and sets buffer_scale = 2.
 * This gives crisp text and UI elements at full Retina quality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

/* arm64: default scale factor (Retina = 2x) */
#define DEFAULT_SCALE  2
#define PANEL_H_LOGICAL  36   /* logical pixels */
#define ICON_SIZE_LOGICAL 64

/* arm64 Retina color scheme — same design, aware of 2x rendering */
static const uint32_t C_PANEL_BG    = 0xE6101010;
static const uint32_t C_PANEL_FG    = 0xFFEEEEEE;
static const uint32_t C_ACCENT      = 0xFF4A90D9;
static const uint32_t C_WALLPAPER_A = 0xFF1a1a2e;
static const uint32_t C_WALLPAPER_B = 0xFF16213e;

/* ── Opaque structs (same as x86_64, added scale field) ─────────────────── */
struct luna_output {
    struct wl_list  link;
    struct wl_output *wl_output;
    int32_t width, height;
    int32_t scale;              /* 1 = normal, 2 = Retina */
    char    name[64];
    struct luna_panel     *panel;
    struct luna_wallpaper *wallpaper;
};

struct luna_shell {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_seat       *seat;
    struct wl_keyboard   *keyboard;
    struct wl_shm        *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;

    struct wl_list outputs;
    struct wl_list toplevels;
    struct wl_list notifications;

    struct luna_launcher *launcher;
    bool   launcher_visible;
    bool   running;

    /* arm64: default scale applied to all buffer sizes */
    int    scale;
};

/* ── Helper: allocate SHM buffer at physical (scaled) size ──────────────── */
static int create_shm_file(size_t size) {
    char name[64];
    snprintf(name, sizeof(name), "/luna-shm-%d-XXXXXX", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

/* ── Pixel fill helpers ──────────────────────────────────────────────────── */
static void fill(uint32_t *px, int stride_px, int x, int y,
                  int w, int h, uint32_t c) {
    for (int r = y; r < y + h; r++)
        for (int col = x; col < x + w; col++)
            px[r * stride_px + col] = c;
}

static void gradient(uint32_t *px, int w, int h, uint32_t ca, uint32_t cb) {
    uint8_t r0=(ca>>16)&0xFF,g0=(ca>>8)&0xFF,b0=ca&0xFF;
    uint8_t r1=(cb>>16)&0xFF,g1=(cb>>8)&0xFF,b1=cb&0xFF;
    for (int y = 0; y < h; y++) {
        float t = (float)y / (h - 1);
        uint32_t c = 0xFF000000
            | ((uint32_t)((uint8_t)(r0 + t*(r1-r0))) << 16)
            | ((uint32_t)((uint8_t)(g0 + t*(g1-g0))) <<  8)
            | ((uint32_t)((uint8_t)(b0 + t*(b1-b0))));
        for (int x = 0; x < w; x++) px[y * w + x] = c;
    }
}

/* ── Output listeners ────────────────────────────────────────────────────── */
static void output_mode(void *data, struct wl_output *o,
    uint32_t flags, int32_t w, int32_t h, int32_t r) {
    (void)o;(void)r;
    struct luna_output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) { out->width = w; out->height = h; }
}
static void output_scale_cb(void *data, struct wl_output *o, int32_t f) {
    (void)o;
    struct luna_output *out = data;
    out->scale = f;
    fprintf(stderr, "luna-shell[arm64]: output scale = %d (Retina = %s)\n",
            f, f >= 2 ? "yes" : "no");
}
static void output_done(void *data, struct wl_output *o) {
    (void)o; (void)data;
    /* Panel + wallpaper created on configure (after layer surface configure) */
}
static void output_geometry(void *d,struct wl_output*o,int32_t x,int32_t y,
    int32_t pw,int32_t ph,int32_t sp,const char*mk,const char*md,int32_t t)
    {(void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sp;(void)mk;(void)md;(void)t;}
static void output_name(void *data, struct wl_output *o, const char *name)
    { (void)o; strncpy(((struct luna_output*)data)->name, name, 63); }
static void output_desc(void *d, struct wl_output *o, const char *desc)
    { (void)d;(void)o;(void)desc; }
static const struct wl_output_listener output_listener = {
    output_geometry, output_mode, output_done, output_scale_cb,
    output_name, output_desc };

/* ── Panel (simplified — renders at physical Retina resolution) ──────────── */
static void create_panel_for_output(struct luna_shell *shell,
                                     struct luna_output *out) {
    /* Physical panel height = logical × scale */
    int phys_h = PANEL_H_LOGICAL * out->scale;
    int phys_w = out->width;      /* physical pixels */

    int stride  = phys_w * 4;
    size_t size = (size_t)(stride * phys_h);
    int fd = create_shm_file(size);
    if (fd < 0) return;

    uint32_t *px = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return; }

    /* Background */
    fill(px, phys_w, 0, 0, phys_w, phys_h, C_PANEL_BG);
    /* Bottom accent border */
    fill(px, phys_w, 0, phys_h - out->scale, phys_w, out->scale, C_ACCENT);

    /* Clock */
    char clock_str[32];
    time_t now = time(NULL);
    strftime(clock_str, sizeof(clock_str), "%H:%M", localtime(&now));
    /* (text rendering stub — replace with freetype in production) */

    /* Launcher button area */
    fill(px, phys_w, 0, 0, 100 * out->scale, phys_h, 0x22FFFFFF);

    struct wl_surface *surf = wl_compositor_create_surface(shell->compositor);
    struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, surf, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "luna-panel");

    zwlr_layer_surface_v1_set_anchor(ls,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(ls, 0, (uint32_t)PANEL_H_LOGICAL);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, PANEL_H_LOGICAL);

    struct wl_shm_pool *pool = wl_shm_create_pool(shell->shm, fd, (int32_t)size);
    struct wl_buffer   *buf  = wl_shm_pool_create_buffer(pool, 0,
        phys_w, phys_h, stride, WL_SHM_FORMAT_ARGB8888);

    /* arm64: set buffer scale for Retina (compositor divides by scale) */
    wl_surface_set_buffer_scale(surf, out->scale);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, phys_w, phys_h);
    wl_surface_commit(surf);

    wl_shm_pool_destroy(pool);
    munmap(px, size);
    close(fd);
    /* Note: buf and surf are owned by the compositor now */
}

static void create_wallpaper_for_output(struct luna_shell *shell,
                                         struct luna_output *out) {
    int phys_w = out->width, phys_h = out->height;
    int stride  = phys_w * 4;
    size_t size = (size_t)(stride * phys_h);
    int fd = create_shm_file(size);
    if (fd < 0) return;
    uint32_t *px = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { close(fd); return; }

    gradient(px, phys_w, phys_h, C_WALLPAPER_A, C_WALLPAPER_B);

    struct wl_surface *surf = wl_compositor_create_surface(shell->compositor);
    struct zwlr_layer_surface_v1 *ls = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, surf, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "luna-wallpaper");

    zwlr_layer_surface_v1_set_anchor(ls,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);

    struct wl_shm_pool *pool = wl_shm_create_pool(shell->shm, fd, (int32_t)size);
    struct wl_buffer   *buf  = wl_shm_pool_create_buffer(pool, 0,
        phys_w, phys_h, stride, WL_SHM_FORMAT_ARGB8888);

    wl_surface_set_buffer_scale(surf, out->scale);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, phys_w, phys_h);
    wl_surface_commit(surf);

    wl_shm_pool_destroy(pool);
    munmap(px, size);
    close(fd);
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */
static void kb_key(void *data, struct wl_keyboard *kb,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)kb;(void)serial;(void)time;
    struct luna_shell *shell = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    /* Super (125/126) or Globe key (arm64: 125 from evdev-bridge) */
    if (key == 125 || key == 126) {
        shell->launcher_visible = !shell->launcher_visible;
        fprintf(stderr, "luna-shell[arm64]: launcher %s\n",
                shell->launcher_visible ? "shown" : "hidden");
    }
    if (key == 1 /* ESC */) shell->launcher_visible = false;
}
static void kb_keymap(void*d,struct wl_keyboard*k,uint32_t f,int32_t fd,uint32_t s)
    {(void)d;(void)k;(void)f;(void)s;close(fd);}
static void kb_enter(void*d,struct wl_keyboard*k,uint32_t s,struct wl_surface*sf,struct wl_array*ks)
    {(void)d;(void)k;(void)s;(void)sf;(void)ks;}
static void kb_leave(void*d,struct wl_keyboard*k,uint32_t s,struct wl_surface*sf)
    {(void)d;(void)k;(void)s;(void)sf;}
static void kb_modifiers(void*d,struct wl_keyboard*k,uint32_t s,uint32_t dm,uint32_t lm,uint32_t g)
    {(void)d;(void)k;(void)s;(void)dm;(void)lm;(void)g;}
static void kb_repeat(void*d,struct wl_keyboard*k,int32_t r,int32_t dl)
    {(void)d;(void)k;(void)r;(void)dl;}
static const struct wl_keyboard_listener kb_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat };

/* ── Seat ────────────────────────────────────────────────────────────────── */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct luna_shell *shell = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !shell->keyboard) {
        shell->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(shell->keyboard, &kb_listener, shell);
    }
}
static void seat_name(void*d,struct wl_seat*s,const char*n){(void)d;(void)s;(void)n;}
static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

/* ── Registry ────────────────────────────────────────────────────────────── */
static void registry_global(void *data, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t version) {
    struct luna_shell *shell = data;
    if (!strcmp(iface, wl_compositor_interface.name))
        shell->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shell->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        shell->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                        version < 7 ? version : 7);
        wl_seat_add_listener(shell->seat, &seat_listener, shell);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        shell->layer_shell = wl_registry_bind(reg, name,
                                &zwlr_layer_shell_v1_interface,
                                version < 4 ? version : 4);
    else if (!strcmp(iface, wl_output_interface.name)) {
        struct luna_output *out = calloc(1, sizeof(*out));
        out->scale = DEFAULT_SCALE;  /* arm64: assume Retina until output_scale says otherwise */
        out->wl_output = wl_registry_bind(reg, name, &wl_output_interface,
                                           version < 4 ? version : 4);
        wl_list_insert(&shell->outputs, &out->link);
        wl_output_add_listener(out->wl_output, &output_listener, out);
    }
}
static void registry_remove(void*d,struct wl_registry*r,uint32_t n){(void)d;(void)r;(void)n;}
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_remove };

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    signal(SIGCHLD, SIG_IGN);

    struct luna_shell shell = {
        .running = true,
        .scale   = DEFAULT_SCALE,
    };
    wl_list_init(&shell.outputs);
    wl_list_init(&shell.toplevels);
    wl_list_init(&shell.notifications);

    shell.display = wl_display_connect(NULL);
    if (!shell.display) {
        fprintf(stderr, "luna-shell[arm64]: cannot connect to Wayland\n");
        return 1;
    }
    shell.registry = wl_display_get_registry(shell.display);
    wl_registry_add_listener(shell.registry, &registry_listener, &shell);
    wl_display_roundtrip(shell.display);
    wl_display_roundtrip(shell.display);

    if (!shell.compositor || !shell.shm || !shell.layer_shell) {
        fprintf(stderr, "luna-shell[arm64]: missing required protocols\n");
        return 1;
    }

    /* Create panel + wallpaper for each output */
    struct luna_output *out;
    wl_list_for_each(out, &shell.outputs, link) {
        wl_display_roundtrip(shell.display); /* get output dimensions */
        if (out->width > 0) {
            create_wallpaper_for_output(&shell, out);
            create_panel_for_output(&shell, out);
        }
    }

    fprintf(stderr, "luna-shell[arm64]: running (scale=%d, Retina=%s)\n",
            shell.scale, shell.scale >= 2 ? "yes" : "no");

    while (shell.running) {
        if (wl_display_dispatch(shell.display) < 0) break;
        /* Redraw panel clock every second */
        static time_t last_sec = 0;
        time_t now = time(NULL);
        if (now != last_sec) {
            last_sec = now;
            struct luna_output *o;
            wl_list_for_each(o, &shell.outputs, link)
                if (o->width > 0) create_panel_for_output(&shell, o);
        }
    }

    wl_display_disconnect(shell.display);
    return 0;
}

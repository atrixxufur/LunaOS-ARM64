/*
 * luna-panel.c — LunaOS top panel (arm64 / Apple Silicon)
 *
 * arm64 HiDPI changes vs x86_64:
 *   - wl_surface_set_buffer_scale(surface, 2) for Retina displays
 *   - Buffer allocated at physical pixels (width*scale × height*scale)
 *   - All draw coordinates are in logical pixels; multiply by scale for buffer
 *   - Font sizes doubled at scale=2 to maintain visual size
 *   - wl_output scale reported via wl_output.scale event, stored in luna_output
 */

#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#define PANEL_FORMAT WL_SHM_FORMAT_ARGB8888
#define PANEL_BPP    4

struct luna_panel {
    struct luna_shell  *shell;
    struct luna_output *output;
    struct wl_surface            *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm_pool *shm_pool;
    struct wl_buffer   *buffer;
    int     width, height;       /* logical pixels */
    int     phys_w, phys_h;      /* physical pixels (×scale for Retina) */
    int     scale;               /* display scale factor */
    bool    configured, dirty;
    uint32_t *pixels;
    int      stride;
    size_t   size;
    int      shm_fd;
    struct { int x, w; } zone_launcher;
    struct { int x, w; } zone_toplevels[32];
    int                  zone_toplevel_count;
};

static void fill(uint32_t *px, int s, int x, int y, int w, int h, uint32_t c) {
    for (int r=y; r<y+h; r++)
        for (int col=x; col<x+w; col++)
            px[r*(s/4)+col] = c;
}

static void draw_text_stub(uint32_t *px, int s, int x, int y,
                            const char *text, uint32_t color, int fsize) {
    int len = (int)strlen(text);
    for (int i=0; i<len; i++)
        fill(px, s, x+i*(fsize/2), y+fsize/4, fsize/2-1, fsize/2, color);
}

static int create_shm(struct luna_panel *p) {
    p->stride = p->phys_w * PANEL_BPP;
    p->size   = (size_t)(p->stride * p->phys_h);
    char name[64]; snprintf(name,64,"/luna-panel-arm64-%d",getpid());
    p->shm_fd = shm_open(name, O_RDWR|O_CREAT|O_EXCL, 0600);
    if (p->shm_fd<0) return -1;
    shm_unlink(name);
    ftruncate(p->shm_fd, (off_t)p->size);
    p->pixels = mmap(NULL, p->size, PROT_READ|PROT_WRITE, MAP_SHARED, p->shm_fd, 0);
    if (p->pixels==MAP_FAILED) { close(p->shm_fd); return -1; }
    p->shm_pool = wl_shm_create_pool(p->shell->shm, p->shm_fd, (int32_t)p->size);
    p->buffer   = wl_shm_pool_create_buffer(p->shm_pool, 0,
                      p->phys_w, p->phys_h, p->stride, PANEL_FORMAT);
    return 0;
}

void luna_panel_render(struct luna_panel *p) {
    if (!p->configured || !p->pixels) return;
    struct luna_shell_config *cfg = &p->shell->config;
    /* Draw at physical resolution */
    int pw = p->phys_w, ph = p->phys_h;
    int sc = p->scale;
    int fsize = cfg->font_size * sc;

    fill(p->pixels, p->stride, 0, 0, pw, ph, cfg->panel_bg_color);
    fill(p->pixels, p->stride, 0, ph-sc, pw, sc, cfg->panel_accent_color);

    int x = 8*sc;

    /* Launcher button */
    int lw = 80*sc;
    fill(p->pixels, p->stride, x, 4*sc, lw, ph-8*sc, 0x224A90D9);
    draw_text_stub(p->pixels, p->stride, x+8*sc, (ph-fsize)/2,
                   "Luna", cfg->panel_fg_color, fsize);
    p->zone_launcher.x = x/sc; p->zone_launcher.w = lw/sc;
    x += lw + 8*sc;

    fill(p->pixels, p->stride, x, 8*sc, sc, ph-16*sc, 0x33FFFFFF);
    x += 8*sc;

    /* Window list */
    p->zone_toplevel_count = 0;
    struct luna_toplevel *tl;
    wl_list_for_each(tl, &p->shell->toplevels, link) {
        if (p->zone_toplevel_count >= 32) break;
        int bw = 140*sc;
        uint32_t bg = tl->activated ? 0x33FFFFFF : 0x11FFFFFF;
        fill(p->pixels, p->stride, x, 4*sc, bw, ph-8*sc, bg);
        draw_text_stub(p->pixels, p->stride, x+8*sc, (ph-fsize)/2,
                       tl->title[0] ? tl->title : tl->app_id,
                       cfg->panel_fg_color, fsize);
        p->zone_toplevels[p->zone_toplevel_count].x = x/sc;
        p->zone_toplevels[p->zone_toplevel_count].w = bw/sc;
        p->zone_toplevel_count++;
        x += bw + 4*sc;
    }

    /* Clock */
    char clk[32]; time_t now=time(NULL);
    strftime(clk, sizeof(clk),
             cfg->clock_format ? cfg->clock_format : "%H:%M",
             localtime(&now));
    int cw = ((int)strlen(clk)*(fsize/2)+16)*sc;
    draw_text_stub(p->pixels, p->stride, pw-cw-8*sc, (ph-fsize)/2,
                   clk, cfg->panel_fg_color, fsize);

    wl_surface_attach(p->surface, p->buffer, 0, 0);
    wl_surface_damage_buffer(p->surface, 0, 0, pw, ph);
    /* arm64 Retina: set buffer scale so compositor scales down correctly */
    wl_surface_set_buffer_scale(p->surface, p->scale);
    wl_surface_commit(p->surface);
    p->dirty = false;
}

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                          uint32_t serial, uint32_t w, uint32_t h) {
    struct luna_panel *p = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    p->width  = (int)w;
    p->height = (int)h;
    /* arm64: allocate physical buffer at Retina resolution */
    p->scale  = p->output->scale > 0 ? p->output->scale : 1;
    p->phys_w = p->width  * p->scale;
    p->phys_h = p->height * p->scale;
    if (p->pixels) {
        munmap(p->pixels, p->size);
        wl_buffer_destroy(p->buffer);
        wl_shm_pool_destroy(p->shm_pool);
        close(p->shm_fd);
    }
    create_shm(p);
    p->configured = true;
    luna_panel_render(p);
}
static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls; ((struct luna_panel*)data)->shell->running=false;
}
static const struct zwlr_layer_surface_v1_listener ls_listener = {
    .configure=ls_configure, .closed=ls_closed
};

struct luna_panel *luna_panel_create(struct luna_shell *shell,
                                     struct luna_output *output) {
    struct luna_panel *p = calloc(1, sizeof(*p));
    p->shell  = shell;
    p->output = output;
    p->height = shell->config.panel_height;
    p->scale  = output->scale > 0 ? output->scale : 1;
    p->surface = wl_compositor_create_surface(shell->compositor);
    p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, p->surface, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "luna-panel");
    zwlr_layer_surface_v1_set_anchor(p->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(p->layer_surface, 0, p->height);
    zwlr_layer_surface_v1_set_exclusive_zone(p->layer_surface, p->height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(p->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(p->layer_surface, &ls_listener, p);
    wl_surface_commit(p->surface);
    return p;
}

void luna_panel_destroy(struct luna_panel *p) {
    if (p->pixels)   munmap(p->pixels, p->size);
    if (p->buffer)   wl_buffer_destroy(p->buffer);
    if (p->shm_pool) wl_shm_pool_destroy(p->shm_pool);
    if (p->shm_fd>=0) close(p->shm_fd);
    zwlr_layer_surface_v1_destroy(p->layer_surface);
    wl_surface_destroy(p->surface);
    free(p);
}

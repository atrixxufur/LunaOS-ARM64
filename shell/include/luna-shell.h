/*
 * luna-shell.h — LunaOS desktop shell (arm64 / Apple Silicon)
 *
 * arm64 notes:
 *   - Retina: wl_surface.set_buffer_scale(2) for HiDPI displays
 *   - Uses os_log() for debug output (visible in Console.app)
 *   - GCD (dispatch_queue) replaces raw pthreads for panel clock timer
 *   - Touch/trackpad events from darwin-evdev-bridge → libinput → wlroots
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

struct luna_shell;
struct luna_panel;
struct luna_launcher;
struct luna_wallpaper;
struct luna_output;
struct luna_toplevel;
struct luna_app;

struct luna_shell_config {
    int      panel_height;
    uint32_t panel_bg_color;
    uint32_t panel_fg_color;
    uint32_t panel_accent_color;
    int      font_size;
    int      launcher_columns;
    int      launcher_icon_size;
    uint32_t launcher_bg_color;
    const char *wallpaper_path;
    uint32_t    wallpaper_color_a;
    uint32_t    wallpaper_color_b;
    const char *clock_format;
    bool        clock_show_seconds;
    int      notif_timeout_ms;
    int      notif_max_visible;
    /* arm64: HiDPI scale factor (1 = standard, 2 = Retina) */
    int      display_scale;
};

struct luna_output {
    struct wl_list      link;
    struct luna_shell  *shell;
    struct wl_output   *wl_output;
    int32_t             width, height, scale;
    char                name[64];
    struct luna_panel      *panel;
    struct luna_wallpaper  *wallpaper;
};

struct luna_toplevel {
    struct wl_list  link;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char   title[256];
    char   app_id[128];
    bool   maximized, minimized, activated;
};

struct luna_app {
    char     name[64];
    char     exec[256];
    char     icon_name[64];
    char     category[32];
    uint32_t accent_color;
};

struct luna_notification {
    struct wl_list  link;
    uint32_t        id;
    char            app_name[64];
    char            summary[128];
    char            body[512];
    int64_t         expire_at_ms;
};

struct luna_shell {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_seat       *seat;
    struct wl_shm        *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;
    struct wl_keyboard   *keyboard;
    struct wl_pointer    *pointer;
    struct wl_list        outputs;
    struct wl_list        toplevels;
    struct wl_list        notifications;
    struct luna_launcher *launcher;
    bool   launcher_visible;
    bool   running;
    uint32_t next_notif_id;
    struct luna_shell_config config;
    struct luna_app *apps;
    int  app_count;
};

struct luna_shell   *luna_shell_create(void);
void                 luna_shell_run(struct luna_shell *shell);
void                 luna_shell_destroy(struct luna_shell *shell);
struct luna_panel   *luna_panel_create(struct luna_shell *shell, struct luna_output *output);
void                 luna_panel_destroy(struct luna_panel *panel);
void                 luna_panel_render(struct luna_panel *panel);
struct luna_launcher *luna_launcher_create(struct luna_shell *shell);
void                  luna_launcher_show(struct luna_launcher *l);
void                  luna_launcher_hide(struct luna_launcher *l);
void                  luna_launcher_destroy(struct luna_launcher *l);
struct luna_wallpaper *luna_wallpaper_create(struct luna_shell *shell, struct luna_output *output);
void                   luna_wallpaper_destroy(struct luna_wallpaper *wp);
uint32_t luna_notify(struct luna_shell *shell, const char *app,
                     const char *summary, const char *body, int timeout_ms);
void     luna_notifyd_tick(struct luna_shell *shell);
int      luna_apps_load(struct luna_shell *shell, const char *dir);
void     luna_app_launch(struct luna_shell *shell, const struct luna_app *app);

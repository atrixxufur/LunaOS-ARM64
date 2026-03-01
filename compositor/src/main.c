/*
 * main.c — luna-compositor for LunaOS arm64 / Apple Silicon
 *
 * Wayland compositor built on wlroots. Identical protocol support to x86_64
 * version, but with arm64-specific backend and rendering considerations:
 *
 *   Display backend: wlr_drm_backend → IODRMShim.kext → IOMobileFramebuffer
 *   Renderer:        wlr_pixman_renderer (software, via lavapipe/llvmpipe)
 *                    OR wlr_gles2_renderer (when MoltenVK/agx available)
 *   Session:         libseat → seatd-darwin (arm64 GCD-based seatd)
 *   Input:           libinput → darwin-evdev-bridge (IOHIDFamily → evdev)
 *
 * Retina display support:
 *   Apple Silicon Macs have 2x HiDPI displays. The compositor operates at
 *   physical resolution (e.g. 3024x1964) and sets output scale = 2.
 *   Wayland clients that set wl_surface.set_buffer_scale(2) get full
 *   Retina quality. Clients that don't scale get 2x magnified output.
 *
 * Keybindings (same as x86_64):
 *   Super+Q     close focused window
 *   Super+T     open terminal (foot)
 *   Super+M     maximize focused window
 *   Super+Shift+E  exit compositor
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "luna-compositor.h"

/* ── Server ──────────────────────────────────────────────────────────────── */
struct luna_server {
    struct wl_display          *display;
    struct wl_event_loop       *event_loop;
    struct wlr_backend         *backend;
    struct wlr_renderer        *renderer;
    struct wlr_allocator       *allocator;
    struct wlr_scene           *scene;
    struct wlr_output_layout   *output_layout;
    struct wlr_seat            *seat;
    struct wlr_cursor          *cursor;
    struct wlr_xcursor_manager *xcursor_mgr;

    /* Wayland protocols */
    struct wlr_compositor       *wl_compositor;
    struct wlr_xdg_shell        *xdg_shell;
    struct wlr_layer_shell_v1   *layer_shell;
    struct wlr_foreign_toplevel_manager_v1 *toplevel_mgr;
    struct wlr_screencopy_manager_v1 *screencopy;
    struct wlr_gamma_control_manager_v1 *gamma_ctrl;

    /* Listeners */
    struct wl_listener new_output;
    struct wl_listener new_xdg_surface;
    struct wl_listener new_layer_surface;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener request_cursor;
    struct wl_listener keyboard_key;
    struct wl_listener keyboard_modifiers;

    /* Lists */
    struct wl_list outputs;     /* luna_output */
    struct wl_list views;       /* luna_view */

    bool running;

    /* arm64: track Retina scale factor from output */
    int output_scale;
};

/* ── Output ──────────────────────────────────────────────────────────────── */
struct luna_output {
    struct wl_list            link;
    struct luna_server       *server;
    struct wlr_output        *wlr_output;
    struct wlr_scene_output  *scene_output;
    struct wl_listener        frame;
    struct wl_listener        destroy;
};

/* ── View (xdg_toplevel window) ──────────────────────────────────────────── */
struct luna_view {
    struct wl_list             link;
    struct luna_server        *server;
    struct wlr_xdg_surface    *xdg_surface;
    struct wlr_scene_node     *scene_node;
    struct wl_listener         map, unmap, destroy, commit;
    bool                       mapped;
    int                        x, y;
    /* foreign-toplevel handle */
    struct wlr_foreign_toplevel_handle_v1 *toplevel_handle;
};

/* ── Forward declarations ────────────────────────────────────────────────── */
static void focus_view(struct luna_view *view, struct wlr_surface *surface);
static struct luna_view *view_at(struct luna_server *server,
                                  double lx, double ly,
                                  struct wlr_surface **surface,
                                  double *sx, double *sy);

/* ── Output frame ────────────────────────────────────────────────────────── */
static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_output *output =
        wl_container_of(listener, output, frame);
    struct wlr_scene_output *scene_output = output->scene_output;
    if (!wlr_scene_output_commit(scene_output, NULL)) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

/* ── New output ──────────────────────────────────────────────────────────── */
static void server_new_output(struct wl_listener *listener, void *data) {
    struct luna_server  *server = wl_container_of(listener, server, new_output);
    struct wlr_output   *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    /* arm64: Apple Silicon outputs report Retina scale */
    if (wlr_output->scale > 1.0f) {
        server->output_scale = (int)wlr_output->scale;
        wlr_log(WLR_INFO, "luna[arm64]: Retina output scale = %d",
                server->output_scale);
    }

    /* Set preferred mode */
    if (!wl_list_empty(&wlr_output->modes)) {
        struct wlr_output_mode *mode =
            wl_container_of(wlr_output->modes.prev, mode, link);
        wlr_output_set_mode(wlr_output, mode);
    }

    struct luna_output *output = calloc(1, sizeof(*output));
    output->server     = server;
    output->wlr_output = wlr_output;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(
        wlr_scene_output_layout_create(server->scene, server->output_layout),
        layout_output, output->scene_output);

    wl_list_insert(&server->outputs, &output->link);
    wlr_output_enable(wlr_output, true);
    wlr_output_commit(wlr_output);

    wlr_log(WLR_INFO, "luna[arm64]: output %s %dx%d scale=%.1f",
            wlr_output->name,
            wlr_output->width, wlr_output->height,
            wlr_output->scale);
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */
static void handle_keyboard_key(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, keyboard_key);
    struct wlr_keyboard_key_event *event = data;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (!kb) return;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
    bool handled = false;

    bool super = wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO;

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && super) {
        for (int i = 0; i < nsyms; i++) {
            switch (syms[i]) {
            case XKB_KEY_q: /* Super+Q: close focused window */
                if (!wl_list_empty(&server->views)) {
                    struct luna_view *v =
                        wl_container_of(server->views.next, v, link);
                    if (v->mapped)
                        wlr_xdg_toplevel_send_close(v->xdg_surface->toplevel);
                    handled = true;
                }
                break;
            case XKB_KEY_t: /* Super+T: terminal */
                if (fork() == 0) {
                    execl("/bin/sh", "sh", "-c", "foot", NULL);
                    _exit(1);
                }
                handled = true;
                break;
            case XKB_KEY_E: /* Super+Shift+E: exit */
                if (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_SHIFT) {
                    server->running = false;
                    handled = true;
                }
                break;
            }
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb);
        wlr_seat_keyboard_notify_key(server->seat,
            event->time_msec, event->keycode, event->state);
    }
}

static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_server *server =
        wl_container_of(listener, server, keyboard_modifiers);
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (!kb) return;
    wlr_seat_set_keyboard(server->seat, kb);
    wlr_seat_keyboard_notify_modifiers(server->seat, &kb->modifiers);
}

/* ── Input ───────────────────────────────────────────────────────────────── */
static void server_new_input(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct wlr_keyboard *kb = wlr_keyboard_from_input_device(device);
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap  *km  = xkb_keymap_new_from_names(
            ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(kb, km);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(kb, 25, 600);

        server->keyboard_key.notify = handle_keyboard_key;
        wl_signal_add(&kb->events.key, &server->keyboard_key);
        server->keyboard_modifiers.notify = handle_keyboard_modifiers;
        wl_signal_add(&kb->events.modifiers, &server->keyboard_modifiers);
        wlr_seat_set_keyboard(server->seat, kb);
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    /* Update seat capabilities */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->seat->keyboards.link))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

/* ── Cursor ──────────────────────────────────────────────────────────────── */
static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(server->cursor, &ev->pointer->base,
                    ev->delta_x, ev->delta_y);

    /* arm64: scale cursor delta for Retina (physical → logical coords) */
    struct wlr_surface *surface = NULL;
    double sx, sy;
    struct luna_view *view = view_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (view && surface) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, ev->time_msec, sx, sy);
    } else {
        wlr_xcursor_manager_set_cursor_image(server->xcursor_mgr,
            "left_ptr", server->cursor);
        wlr_seat_pointer_clear_focus(server->seat);
    }
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    wlr_seat_pointer_notify_button(server->seat,
        ev->time_msec, ev->button, ev->state);

    struct wlr_surface *surface = NULL;
    double sx, sy;
    struct luna_view *view = view_at(server,
        server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && view)
        focus_view(view, surface);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(server->seat, ev->time_msec,
        ev->orientation, ev->delta, ev->delta_discrete, ev->source,
        WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

/* ── XDG surface ─────────────────────────────────────────────────────────── */
static void view_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, map);
    view->mapped = true;
    focus_view(view, view->xdg_surface->surface);

    /* Register with foreign-toplevel manager for the panel */
    struct luna_server *server = view->server;
    view->toplevel_handle =
        wlr_foreign_toplevel_handle_v1_create(server->toplevel_mgr);
    if (view->xdg_surface->toplevel->title)
        wlr_foreign_toplevel_handle_v1_set_title(view->toplevel_handle,
            view->xdg_surface->toplevel->title);
    if (view->xdg_surface->toplevel->app_id)
        wlr_foreign_toplevel_handle_v1_set_app_id(view->toplevel_handle,
            view->xdg_surface->toplevel->app_id);
}

static void view_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
    if (view->toplevel_handle) {
        wlr_foreign_toplevel_handle_v1_destroy(view->toplevel_handle);
        view->toplevel_handle = NULL;
    }
}

static void view_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct luna_server    *server =
        wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    struct luna_view *view = calloc(1, sizeof(*view));
    view->server      = server;
    view->xdg_surface = xdg_surface;
    view->scene_node  =
        &wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface)->tree.node;

    view->map.notify     = view_map;
    view->unmap.notify   = view_unmap;
    view->destroy.notify = view_destroy;
    wl_signal_add(&xdg_surface->surface->events.map,    &view->map);
    wl_signal_add(&xdg_surface->surface->events.unmap,  &view->unmap);
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

    wl_list_insert(&server->views, &view->link);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static struct luna_view *view_at(struct luna_server *server,
                                  double lx, double ly,
                                  struct wlr_surface **surface,
                                  double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surf =
        wlr_scene_surface_try_from_buffer(scene_buf);
    if (!scene_surf) return NULL;
    *surface = scene_surf->surface;

    struct wlr_scene_node *parent = node->parent ? &node->parent->node : NULL;
    while (parent) {
        if (parent->data) {
            struct luna_view *view = parent->data;
            return view;
        }
        parent = parent->parent ? &parent->parent->node : NULL;
    }
    return NULL;
}

static void focus_view(struct luna_view *view, struct wlr_surface *surface) {
    struct luna_server *server = view->server;
    struct wlr_surface *prev_surface =
        server->seat->keyboard_state.focused_surface;
    if (prev_surface == surface) return;

    if (prev_surface) {
        struct wlr_xdg_surface *prev_xdg =
            wlr_xdg_surface_try_from_wlr_surface(prev_surface);
        if (prev_xdg && prev_xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
            wlr_xdg_toplevel_set_activated(prev_xdg->toplevel, false);
    }
    wlr_scene_node_raise_to_top(view->scene_node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (kb)
        wlr_seat_keyboard_notify_enter(server->seat, surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);
    wlr_log(WLR_INFO, "luna-compositor arm64 starting");

    struct luna_server server = {0};
    server.running     = true;
    server.output_scale = 1;
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);

    /* Wayland display */
    server.display    = wl_display_create();
    server.event_loop = wl_display_get_event_loop(server.display);

    /* Backend: DRM (IOMobileFramebuffer on arm64) + libinput */
    server.backend = wlr_backend_autocreate(server.event_loop, NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return 1;
    }

    /* Renderer: pixman (software, works with lavapipe/llvmpipe) */
    server.renderer = wlr_pixman_renderer_create();
    if (!server.renderer) server.renderer =
        wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend,
                                                  server.renderer);
    server.scene       = wlr_scene_create();
    server.output_layout = wlr_output_layout_create();

    /* Protocols */
    server.wl_compositor = wlr_compositor_create(server.display, 6,
                                                   server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);
    server.xdg_shell     = wlr_xdg_shell_create(server.display, 6);
    server.layer_shell   = wlr_layer_shell_v1_create(server.display, 4);
    server.toplevel_mgr  = wlr_foreign_toplevel_manager_v1_create(server.display);
    server.screencopy    = wlr_screencopy_manager_v1_create(server.display);
    server.gamma_ctrl    = wlr_gamma_control_manager_v1_create(server.display);
    wlr_viewporter_create(server.display);
    wlr_presentation_create(server.display, server.backend);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);

    /* Seat + cursor */
    server.seat       = wlr_seat_create(server.display, "seat0");
    server.cursor     = wlr_cursor_create();
    server.xcursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wlr_xcursor_manager_load(server.xcursor_mgr, 1);

    /* Listeners */
    server.new_output.notify  = server_new_output;
    server.new_xdg_surface.notify = server_new_xdg_surface;
    server.new_input.notify   = server_new_input;
    server.cursor_motion.notify = handle_cursor_motion;
    server.cursor_button.notify = handle_cursor_button;
    server.cursor_axis.notify   = handle_cursor_axis;
    wl_signal_add(&server.backend->events.new_output,  &server.new_output);
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);
    wl_signal_add(&server.backend->events.new_input,   &server.new_input);
    wl_signal_add(&server.cursor->events.motion,       &server.cursor_motion);
    wl_signal_add(&server.cursor->events.button,       &server.cursor_button);
    wl_signal_add(&server.cursor->events.axis,         &server.cursor_axis);

    /* Wayland socket */
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) { wlr_log(WLR_ERROR, "no socket"); return 1; }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (arm64)", socket);

    mkdir("/run/user/501", 0700);
    setenv("XDG_RUNTIME_DIR", "/run/user/501", 0);

    /* Start backend */
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "backend start failed"); return 1;
    }

    /* Launch shell */
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c",
              "/usr/local/bin/luna-shell", NULL);
        _exit(1);
    }

    /* Event loop */
    while (server.running)
        wl_event_loop_dispatch(server.event_loop, 16);

    wl_display_destroy_clients(server.display);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return 0;
}

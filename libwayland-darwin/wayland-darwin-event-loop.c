/*
 * wayland-darwin-event-loop.c — Wayland event loop integration for Darwin arm64
 *
 * Integrates the Wayland event loop (wl_event_loop) with Darwin's native
 * event mechanisms. On arm64 Apple Silicon we prefer GCD (Grand Central
 * Dispatch) over raw kqueue because GCD integrates with the QoS scheduler,
 * giving input/display events the correct priority class.
 *
 * Strategy:
 *   wl_event_loop uses an internal epoll-like fd set.
 *   Darwin has no epoll — we substitute kqueue.
 *   On arm64, we wrap kqueue sources in dispatch_source_t so the OS
 *   can schedule them efficiently on the efficiency/performance cores.
 *
 * Priority mapping (arm64 QoS → Wayland event classes):
 *   QOS_CLASS_USER_INTERACTIVE  → display vblank, input events
 *   QOS_CLASS_USER_INITIATED    → Wayland protocol messages
 *   QOS_CLASS_UTILITY           → background compositor tasks
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <dispatch/dispatch.h>
#include <os/log.h>

/* Wayland event loop fd mask (matching wayland-server.h) */
#define WL_EVENT_READABLE  0x01
#define WL_EVENT_WRITABLE  0x02
#define WL_EVENT_HANGUP    0x04
#define WL_EVENT_ERROR     0x08

#define MAX_SOURCES 256

typedef int (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef int (*wl_event_loop_signal_func_t)(int signo, void *data);

struct wl_event_source {
    int                       fd;
    uint32_t                  mask;
    wl_event_loop_fd_func_t   fd_func;
    wl_event_loop_timer_func_t timer_func;
    void                     *data;

    /* arm64: GCD dispatch source */
    dispatch_source_t         dispatch_src;
    bool                      is_timer;
    bool                      cancelled;
};

struct wl_event_loop {
    int                    kq;              /* kqueue fd */
    dispatch_queue_t       queue;           /* arm64: GCD queue */
    struct wl_event_source sources[MAX_SOURCES];
    int                    source_count;

    /* Self-pipe for wl_event_loop_interrupt() */
    int                    pipe_r, pipe_w;
};

/* ── Create event loop ────────────────────────────────────────────────────── */
struct wl_event_loop *wl_event_loop_create(void) {
    struct wl_event_loop *loop = calloc(1, sizeof(*loop));
    loop->kq = kqueue();

    /* arm64: create a high-priority concurrent queue for Wayland events */
    loop->queue = dispatch_queue_create_with_target(
        "org.lunaos.wayland.eventloop",
        DISPATCH_QUEUE_CONCURRENT,
        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));

    /* Self-pipe for wakeup */
    int fds[2];
    pipe(fds);
    loop->pipe_r = fds[0];
    loop->pipe_w = fds[1];
    fcntl(loop->pipe_r, F_SETFL, O_NONBLOCK);
    fcntl(loop->pipe_w, F_SETFL, O_NONBLOCK);

    return loop;
}

void wl_event_loop_destroy(struct wl_event_loop *loop) {
    for (int i = 0; i < loop->source_count; i++) {
        if (loop->sources[i].dispatch_src) {
            dispatch_source_cancel(loop->sources[i].dispatch_src);
            dispatch_release(loop->sources[i].dispatch_src);
        }
    }
    dispatch_release(loop->queue);
    close(loop->kq);
    close(loop->pipe_r);
    close(loop->pipe_w);
    free(loop);
}

/* ── Add fd source ────────────────────────────────────────────────────────── */
struct wl_event_source *wl_event_loop_add_fd(
        struct wl_event_loop *loop, int fd, uint32_t mask,
        wl_event_loop_fd_func_t func, void *data) {

    if (loop->source_count >= MAX_SOURCES) return NULL;
    struct wl_event_source *src = &loop->sources[loop->source_count++];
    src->fd      = fd;
    src->mask    = mask;
    src->fd_func = func;
    src->data    = data;

    /* arm64: use dispatch_source for read events (USER_INTERACTIVE QoS) */
    if (mask & WL_EVENT_READABLE) {
        src->dispatch_src = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, loop->queue);

        dispatch_source_set_event_handler(src->dispatch_src, ^{
            if (!src->cancelled)
                func(fd, WL_EVENT_READABLE, data);
        });
        dispatch_source_set_cancel_handler(src->dispatch_src, ^{
            src->cancelled = true;
        });
        dispatch_resume(src->dispatch_src);
    } else if (mask & WL_EVENT_WRITABLE) {
        /* kqueue for write readiness (less common) */
        struct kevent kev;
        EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, src);
        kevent(loop->kq, &kev, 1, NULL, 0, NULL);
    }
    return src;
}

/* ── Add timer ────────────────────────────────────────────────────────────── */
struct wl_event_source *wl_event_loop_add_timer(
        struct wl_event_loop *loop,
        wl_event_loop_timer_func_t func, void *data) {

    if (loop->source_count >= MAX_SOURCES) return NULL;
    struct wl_event_source *src = &loop->sources[loop->source_count++];
    src->is_timer   = true;
    src->timer_func = func;
    src->data       = data;

    /* arm64: dispatch_source timer (coalesced by OS for energy efficiency) */
    src->dispatch_src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));

    dispatch_source_set_event_handler(src->dispatch_src, ^{
        if (!src->cancelled) func(data);
    });
    /* Don't resume yet — caller sets interval via wl_event_source_timer_update */
    return src;
}

int wl_event_source_timer_update(struct wl_event_source *src, int ms) {
    if (!src->dispatch_src) return -1;
    uint64_t interval = (uint64_t)ms * NSEC_PER_MSEC;
    dispatch_source_set_timer(src->dispatch_src,
        dispatch_time(DISPATCH_TIME_NOW, interval),
        interval, interval / 10 /* 10% leeway for energy efficiency */);
    if (ms > 0 && !src->cancelled)
        dispatch_resume(src->dispatch_src);
    return 0;
}

/* ── Remove source ────────────────────────────────────────────────────────── */
void wl_event_source_remove(struct wl_event_source *src) {
    src->cancelled = true;
    if (src->dispatch_src) {
        dispatch_source_cancel(src->dispatch_src);
        dispatch_release(src->dispatch_src);
        src->dispatch_src = NULL;
    }
}

/* ── Dispatch (called from compositor main loop) ──────────────────────────── */
int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout_ms) {
    /*
     * arm64: GCD handles most event dispatch asynchronously.
     * This function just needs to wait for something to happen
     * and drain the kqueue for write-ready events.
     */
    struct timespec ts = {
        .tv_sec  = timeout_ms / 1000,
        .tv_nsec = (timeout_ms % 1000) * 1000000L,
    };
    struct kevent events[32];
    int n = kevent(loop->kq, NULL, 0, events, 32,
                   timeout_ms < 0 ? NULL : &ts);
    for (int i = 0; i < n; i++) {
        struct wl_event_source *src = events[i].udata;
        if (!src || src->cancelled) continue;
        uint32_t mask = 0;
        if (events[i].filter == EVFILT_WRITE) mask |= WL_EVENT_WRITABLE;
        if (events[i].flags & EV_EOF)         mask |= WL_EVENT_HANGUP;
        if (events[i].flags & EV_ERROR)       mask |= WL_EVENT_ERROR;
        if (mask && src->fd_func)
            src->fd_func(src->fd, mask, src->data);
    }
    return 0;
}

void wl_event_loop_dispatch_idle(struct wl_event_loop *loop) {
    (void)loop;
    /* GCD handles idle tasks automatically */
}

int wl_event_loop_get_fd(struct wl_event_loop *loop) {
    return loop->kq;
}

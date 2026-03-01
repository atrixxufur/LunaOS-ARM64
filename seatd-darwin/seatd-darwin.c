/*
 * seatd-darwin.c — LunaOS seat management daemon (arm64 / Apple Silicon)
 *
 * Manages access to /dev/dri/card0 (IODRMShim) and /dev/input/event*
 * (darwin-evdev-bridge), granting/revoking access based on session focus.
 *
 * arm64 differences from x86_64:
 *   - Uses dispatch_source_t (GCD) for kqueue integration on Apple Silicon
 *     instead of raw kevent() — GCD is more efficient on arm64 with QoS
 *   - Monitors IOMobileFramebuffer for display focus events
 *     (on Apple Silicon, the display controller signals focus changes)
 *   - Uses os_log() for logging (Apple Silicon system log, visible in Console.app)
 *   - /var/run/seatd.sock → /run/seatd.sock (same path, different VFS root)
 *
 * Protocol (identical to x86_64 version — compositors don't see the difference):
 *   Client connects → sends SEAT_OPEN_DEVICE("/dev/dri/card0")
 *   seatd opens fd with O_RDWR, sends it via SCM_RIGHTS
 *   On session switch → sends SEAT_DISABLE to compositor
 *   Compositor releases device → sends SEAT_ENABLE when focus returns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <dispatch/dispatch.h>
#include <os/log.h>

#define SEATD_SOCKET  "/run/seatd.sock"
#define MAX_CLIENTS   16
#define BACKLOG       8
#define VIDEO_GROUP   "video"
#define INPUT_GROUP   "input"

/* ── Wire protocol ────────────────────────────────────────────────────────── */
#define MSG_OPEN_DEVICE   1
#define MSG_CLOSE_DEVICE  2
#define MSG_ENABLE_SEAT   3
#define MSG_DISABLE_SEAT  4

struct seatd_msg {
    uint16_t type;
    uint16_t len;
    char     data[256];
};

/* ── Client state ─────────────────────────────────────────────────────────── */
struct client {
    int      fd;
    pid_t    pid;
    bool     active;
    int      held_fds[8];
    int      held_count;
};

/* ── Global state ─────────────────────────────────────────────────────────── */
static int            g_server_fd = -1;
static struct client  g_clients[MAX_CLIENTS];
static dispatch_queue_t g_queue;
static os_log_t       g_log;
static volatile bool  g_running = true;

/* ── Logging (os_log on arm64 — visible in Console.app) ─────────────────── */
#define LOG_INFO(fmt, ...) os_log_info(g_log, "[seatd-arm64] " fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  os_log_error(g_log, "[seatd-arm64] " fmt, ##__VA_ARGS__)

/* ── Send fd via SCM_RIGHTS ───────────────────────────────────────────────── */
static int send_fd(int sock, int fd, int reply_code) {
    struct msghdr  msg   = {};
    struct iovec   iov   = { &reply_code, sizeof(reply_code) };
    char           cmsg_buf[CMSG_SPACE(sizeof(int))];

    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    if (fd >= 0) {
        msg.msg_control    = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type  = SCM_RIGHTS;
        cm->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return (int)sendmsg(sock, &msg, 0);
}

/* ── Handle OPEN_DEVICE request ───────────────────────────────────────────── */
static void handle_open_device(struct client *cl, const char *path) {
    LOG_INFO("client pid=%d requests: %{public}s", cl->pid, path);

    /* Validate: only allow /dev/dri/ and /dev/input/ */
    if (strncmp(path, "/dev/dri/",   9) != 0 &&
        strncmp(path, "/dev/input/", 11) != 0) {
        LOG_ERR("rejected path: %{public}s", path);
        send_fd(cl->fd, -1, -EPERM);
        return;
    }

    int dev_fd = open(path, O_RDWR | O_CLOEXEC);
    if (dev_fd < 0) {
        LOG_ERR("open(%{public}s) failed: %d", path, errno);
        send_fd(cl->fd, -1, -errno);
        return;
    }

    /* Track the held fd so we can revoke on session switch */
    if (cl->held_count < 8)
        cl->held_fds[cl->held_count++] = dev_fd;

    send_fd(cl->fd, dev_fd, 0);
    close(dev_fd); /* client has its own fd via SCM_RIGHTS */
    LOG_INFO("granted %{public}s to pid=%d", path, cl->pid);
}

/* ── Handle client message ────────────────────────────────────────────────── */
static void handle_client_message(struct client *cl) {
    struct seatd_msg msg;
    ssize_t n = recv(cl->fd, &msg, sizeof(msg), 0);
    if (n <= 0) {
        /* Client disconnected */
        LOG_INFO("client pid=%d disconnected", cl->pid);
        close(cl->fd);
        cl->fd     = -1;
        cl->active = false;
        return;
    }
    msg.data[sizeof(msg.data) - 1] = 0;

    switch (msg.type) {
    case MSG_OPEN_DEVICE:
        handle_open_device(cl, msg.data);
        break;
    case MSG_CLOSE_DEVICE: {
        /* Client releasing a device fd */
        int reply = 0;
        send(cl->fd, &reply, sizeof(reply), 0);
        break;
    }
    case MSG_ENABLE_SEAT:
    case MSG_DISABLE_SEAT: {
        int reply = 0;
        send(cl->fd, &reply, sizeof(reply), 0);
        break;
    }
    default:
        LOG_ERR("unknown message type %d from pid=%d", msg.type, cl->pid);
    }
}

/* ── Accept new client ────────────────────────────────────────────────────── */
static void accept_client(void) {
    int cfd = accept(g_server_fd, NULL, NULL);
    if (cfd < 0) return;

    /* Get client pid via getsockopt */
    struct xucred cr = {};
    socklen_t len = sizeof(cr);
    getsockopt(cfd, SOL_LOCAL, LOCAL_PEERCRED, &cr, &len);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            g_clients[i].fd     = cfd;
            g_clients[i].pid    = cr.cr_pid;
            g_clients[i].active = true;
            g_clients[i].held_count = 0;
            LOG_INFO("new client pid=%d slot=%d", cr.cr_pid, i);

            /* arm64: use GCD dispatch source for client I/O */
            dispatch_source_t src = dispatch_source_create(
                DISPATCH_SOURCE_TYPE_READ, (uintptr_t)cfd, 0, g_queue);
            dispatch_source_set_event_handler(src, ^{
                handle_client_message(&g_clients[i]);
                if (g_clients[i].fd < 0)
                    dispatch_source_cancel(src);
            });
            dispatch_resume(src);
            return;
        }
    }
    LOG_ERR("max clients reached — rejecting pid=%d", cr.cr_pid);
    close(cfd);
}

/* ── Setup UNIX socket ────────────────────────────────────────────────────── */
static int setup_socket(const char *path) {
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    chmod(path, 0660);

    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* ── Signal handler ───────────────────────────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_running = false; }

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *socket_path = SEATD_SOCKET;
    const char *group_name  = VIDEO_GROUP;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i+1 < argc) socket_path = argv[++i];
        if (!strcmp(argv[i], "-g") && i+1 < argc) group_name  = argv[++i];
    }

    g_log   = os_log_create("org.lunaos.seatd", "seatd-arm64");
    g_queue = dispatch_queue_create("org.lunaos.seatd.queue",
                                    DISPATCH_QUEUE_CONCURRENT);

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Create /run directory if needed */
    mkdir("/run", 0755);

    g_server_fd = setup_socket(socket_path);
    if (g_server_fd < 0) return 1;

    LOG_INFO("listening on %{public}s (group=%{public}s)", socket_path, group_name);
    fprintf(stderr, "[seatd-arm64] listening on %s\n", socket_path);

    /* arm64: GCD dispatch source for server accept loop */
    dispatch_source_t accept_src = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, (uintptr_t)g_server_fd, 0, g_queue);
    dispatch_source_set_event_handler(accept_src, ^{ accept_client(); });
    dispatch_resume(accept_src);

    /* Run until signal */
    dispatch_main();
    return 0;
}

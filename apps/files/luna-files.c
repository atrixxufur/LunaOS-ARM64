/*
 * luna-files.c — LunaOS file manager (arm64 / Apple Silicon)
 *
 * Identical logic to x86_64 version.
 * arm64-specific compile flags are handled by CMakeLists.txt (-arch arm64).
 * No source changes needed — Darwin arm64 userspace ABI is identical
 * to x86_64 for standard POSIX APIs used here (opendir, stat, fork, etc.)
 *
 * Retina note: the shell renders panels at 2x; luna-files as an xdg_toplevel
 * client should set wl_surface.set_buffer_scale(2) for crisp Retina rendering.
 * For now we let the compositor scale up the window (acceptable quality).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

#define MAX_FILES 4096
#define ROW_H     28
#define FONT_SZ   13

typedef struct {
    char  name[256];
    char  path[512];
    int   is_dir;
    off_t size;
    time_t mtime;
} FileEntry;

typedef struct {
    struct wl_display    *display;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    bool  running;
    char  cwd[512];
    FileEntry files[MAX_FILES];
    int   file_count, selected, scroll;
} FilesApp;

static int cmp_entries(const void *a, const void *b) {
    const FileEntry *fa = a, *fb = b;
    if (fa->is_dir != fb->is_dir) return fa->is_dir ? -1 : 1;
    return strcasecmp(fa->name, fb->name);
}

static void load_dir(FilesApp *app, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    strncpy(app->cwd, path, sizeof(app->cwd)-1);
    app->file_count = app->selected = app->scroll = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && app->file_count < MAX_FILES) {
        if (!strcmp(ent->d_name, ".")) continue;
        FileEntry *e = &app->files[app->file_count++];
        strncpy(e->name, ent->d_name, 255);
        snprintf(e->path, 511, "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(e->path, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size   = st.st_size;
            e->mtime  = st.st_mtime;
        }
    }
    closedir(d);
    qsort(app->files, app->file_count, sizeof(FileEntry), cmp_entries);
}

int main(int argc, char *argv[]) {
    const char *start = argc > 1 ? argv[1] : (getenv("HOME") ?: "/");
    FilesApp app = { .running = true };
    load_dir(&app, start);
    fprintf(stderr, "luna-files[arm64]: %d items in %s\n",
            app.file_count, app.cwd);
    /* Full Wayland client init omitted — same pattern as luna-shell-main.c */
    return 0;
}

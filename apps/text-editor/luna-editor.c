/*
 * luna-editor.c — LunaOS text editor (arm64 / Apple Silicon)
 *
 * Same source as x86_64 version — all standard C, no arch-specific code.
 * arm64 compile flags set in CMakeLists.txt.
 *
 * On Apple Silicon the editor runs at full Retina quality when the
 * compositor applies buffer_scale=2 scaling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#define MAX_LINES 65536
#define MAX_LINE  4096

typedef struct { char *data; int len, cap; } Line;

typedef struct {
    Line  lines[MAX_LINES];
    int   line_count, cursor_row, cursor_col, scroll_row;
    char  filepath[512];
    bool  modified, running;
    /* Wayland omitted */
} Editor;

static void line_init(Line *l) {
    l->cap = 80; l->data = calloc(l->cap, 1); l->len = 0;
}

static void editor_load(Editor *ed, const char *path) {
    FILE *f = fopen(path, "r");
    strncpy(ed->filepath, path, 511);
    ed->line_count = 0;
    if (!f) { ed->line_count = 1; line_init(&ed->lines[0]); return; }
    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), f) && ed->line_count < MAX_LINES) {
        Line *l = &ed->lines[ed->line_count++];
        line_init(l);
        int n = (int)strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[--n] = 0;
        memcpy(l->data, buf, n+1); l->len = n;
    }
    if (!ed->line_count) { ed->line_count=1; line_init(&ed->lines[0]); }
    fclose(f);
}

int main(int argc, char *argv[]) {
    Editor *ed = calloc(1, sizeof(*ed));
    ed->running = true;
    if (argc > 1) editor_load(ed, argv[1]);
    else { ed->line_count = 1; line_init(&ed->lines[0]); }
    fprintf(stderr, "luna-editor[arm64]: %s (%d lines)\n",
            ed->filepath[0] ? ed->filepath : "[new]", ed->line_count);
    free(ed);
    return 0;
}

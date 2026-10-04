// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_TEST_HELPERS_H
#define SIGIL_TEST_HELPERS_H

#include "sigil.h"
#include "test_fs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SKIP 77

static const char *get_rom_dir(void) {
    const char *d = getenv("SIGIL_ROM_DIR");
    if (!d || !*d) return NULL;
    return d;
}

static int build_subdir(const char *rom_dir, const char *sub, char out[512]) {
    int n = snprintf(out, 512, "%s/%s", rom_dir, sub);
    return (n > 0 && n < 512) ? 0 : -1;
}

static void ext_of(const char *name, char out[16]) {
    out[0] = '\0';
    const char *dot = strrchr(name, '.');
    if (!dot) return;
    dot++;
    size_t i = 0;
    while (dot[i] && i < 15) {
        out[i] = (char)((dot[i] >= 'A' && dot[i] <= 'Z') ? dot[i] + 32 : dot[i]);
        i++;
    }
    out[i] = '\0';
}

typedef struct {
    int processed;
    int passed;
    int failed;
} walk_stats;

typedef int (*walk_fn)(const char *full_path, const char *basename);

static int sample_limit_from_env(int default_limit) {
    const char *v = getenv("SIGIL_SAMPLE_LIMIT");
    if (!v) return default_limit;
    int n = atoi(v);
    return n > 0 ? n : default_limit;
}

typedef struct {
    const char  *dir;
    walk_fn      fn;
    const char **filter_exts;
    int          limit;
    walk_stats   st;
} walk_ctx;

static bool walk_entry(void *ctx, const char *name, bool is_dir) {
    walk_ctx *w = (walk_ctx *)ctx;
    if (w->st.processed >= w->limit) return false;
    if (name[0] == '.' || is_dir) return true;

    char ext[16];
    ext_of(name, ext);
    if (w->filter_exts) {
        bool match = false;
        for (int i = 0; w->filter_exts[i]; i++) {
            if (strcmp(ext, w->filter_exts[i]) == 0) { match = true; break; }
        }
        if (!match) return true;
    }

    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", w->dir, name);
    w->st.processed++;
    if (w->fn(full, name) == 0) w->st.passed++;
    else                        w->st.failed++;
    return true;
}

static walk_stats walk_dir(const char *dir, walk_fn fn, const char *filter_exts[]) {
    walk_ctx w = { dir, fn, filter_exts, sample_limit_from_env(25), {0} };
    test_dir_each(dir, walk_entry, &w);
    return w.st;
}

#endif

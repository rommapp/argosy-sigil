// SPDX-License-Identifier: MPL-2.0
/* An in-memory save root for the sync tests: the files a client would list,
 * with the open and write callbacks sigil calls. */
#ifndef SIGIL_TEST_MEM_ROOT_H
#define SIGIL_TEST_MEM_ROOT_H

#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_ROOT_FILES 48

typedef struct {
    char     path[SIGIL_SAVE_PATH_MAX];
    uint8_t *data;
    size_t   len;
} mem_file;

typedef struct {
    mem_file    files[MEM_ROOT_FILES];
    size_t      count;
    int         writes;
    int         removes;
    const char *listing[MEM_ROOT_FILES];
} mem_root;

static mem_file *root_find(mem_root *r, const char *path) {
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->files[i].path, path) == 0) return &r->files[i];
    }
    return NULL;
}

static void root_put(mem_root *r, const char *path, const uint8_t *data, size_t len) {
    mem_file *f = root_find(r, path);
    if (!f) {
        if (r->count >= MEM_ROOT_FILES) {
            fprintf(stderr, "mem_root: more than %d files\n", MEM_ROOT_FILES);
            abort();
        }
        f = &r->files[r->count++];
        snprintf(f->path, sizeof(f->path), "%s", path);
    } else {
        free(f->data);
    }
    f->data = (uint8_t *)malloc(len);
    memcpy(f->data, data, len);
    f->len = len;
    for (size_t i = 0; i < r->count; i++) r->listing[i] = r->files[i].path;
}

static void root_free(mem_root *r) {
    for (size_t i = 0; i < r->count; i++) free(r->files[i].data);
    memset(r, 0, sizeof(*r));
}

typedef struct { const uint8_t *data; size_t len; } mem_root_view;

static int mem_root_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_root_view *m = (mem_root_view *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}
static int64_t mem_root_size(void *ctx) { return (int64_t)((mem_root_view *)ctx)->len; }
static void mem_root_close(void *ctx) { free(ctx); }

/* A sigil_io over `len` bytes the caller keeps alive. */
static sigil_io *mem_root_io(const uint8_t *data, size_t len) {
    mem_root_view *m = (mem_root_view *)malloc(sizeof(*m));
    sigil_io *io = (sigil_io *)malloc(sizeof(*io));
    m->data = data;
    m->len = len;
    io->read = mem_root_read;
    io->size = mem_root_size;
    io->close = mem_root_close;
    io->ctx = m;
    return io;
}

static sigil_io *root_open(void *ctx, const char *path) {
    mem_file *f = root_find((mem_root *)ctx, path);
    return f ? mem_root_io(f->data, f->len) : NULL;
}

static int root_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    mem_root *r = (mem_root *)ctx;
    root_put(r, path, data, len);
    r->writes++;
    return 0;
}

static int root_remove(void *ctx, const char *path) {
    mem_root *r = (mem_root *)ctx;
    mem_file *f = root_find(r, path);
    if (!f) return -1;
    free(f->data);
    size_t at = (size_t)(f - r->files);
    memmove(&r->files[at], &r->files[at + 1], (r->count - at - 1) * sizeof(mem_file));
    r->count--;
    memset(&r->files[r->count], 0, sizeof(mem_file));
    for (size_t i = 0; i < r->count; i++) r->listing[i] = r->files[i].path;
    r->removes++;
    return 0;
}

#endif

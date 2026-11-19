// SPDX-License-Identifier: MPL-2.0
/* An in-memory save root for the sync tests: the files a client would list,
 * with the open and write callbacks sigil calls. */
#ifndef SIGIL_TEST_MEM_ROOT_H
#define SIGIL_TEST_MEM_ROOT_H

#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_ROOT_FILES 160

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
    int         dirs_removed;
    int         fail_write;      /* the write (1-based) that reports failure; 0 for none */
    int         corrupt_write;   /* the write (1-based) stored with one byte flipped; 0 for none */
    size_t      corrupt_at;      /* the byte it flips; 0 for the last */
    char        first_write[SIGIL_SAVE_PATH_MAX];   /* the path the first write went to */
    char        unreadable[SIGIL_SAVE_PATH_MAX];    /* a listed path open refuses, as a client's can */
    const char *listing[MEM_ROOT_FILES];
    int         parts;           /* writes of part of a file */
    size_t      part_bytes;      /* the bytes they carried */
    int         fail_part;       /* the part write (1-based) that reports failure; 0 for none */
    int         torn;            /* 1 for fail_part to store its last 4 KiB first, as a disk can before a power cut */
    int         corrupt_part;    /* the part write (1-based) stored with its last byte flipped; 0 for none */
    int         in_parts;        /* part writes since the last open: together they count as one write */
    int         open_streams;    /* streams root_open gave that aren't closed yet */
    int         exclusive;       /* 1 to refuse a write while a stream is open, as a client that replaces
                                    the file, or a system that locks an open one, would */
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

typedef struct { const uint8_t *data; size_t len; int *open_streams; } mem_root_view;

static int mem_root_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_root_view *m = (mem_root_view *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}
static int64_t mem_root_size(void *ctx) { return (int64_t)((mem_root_view *)ctx)->len; }
static void mem_root_close(void *ctx) {
    mem_root_view *m = (mem_root_view *)ctx;
    if (m->open_streams) (*m->open_streams)--;
    free(ctx);
}

/* A sigil_io over `len` bytes the caller keeps alive. */
static sigil_io *mem_root_io(const uint8_t *data, size_t len) {
    mem_root_view *m = (mem_root_view *)malloc(sizeof(*m));
    sigil_io *io = (sigil_io *)malloc(sizeof(*io));
    m->data = data;
    m->len = len;
    m->open_streams = NULL;
    io->read = mem_root_read;
    io->size = mem_root_size;
    io->close = mem_root_close;
    io->ctx = m;
    return io;
}

static sigil_io *root_open(void *ctx, const char *path) {
    mem_root *r = (mem_root *)ctx;
    r->in_parts = 0;
    if (r->unreadable[0] && strcmp(r->unreadable, path) == 0) return NULL;
    mem_file *f = root_find(r, path);
    if (!f) return NULL;
    sigil_io *io = mem_root_io(f->data, f->len);
    ((mem_root_view *)io->ctx)->open_streams = &r->open_streams;
    r->open_streams++;
    return io;
}

static int root_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    mem_root *r = (mem_root *)ctx;
    r->writes++;
    if (r->writes == 1) snprintf(r->first_write, sizeof(r->first_write), "%s", path);
    if (r->writes == r->fail_write || (r->exclusive && r->open_streams)) return -1;
    root_put(r, path, data, len);
    if (r->writes == r->corrupt_write && len > 0) {
        root_find(r, path)->data[r->corrupt_at && r->corrupt_at < len ? r->corrupt_at : len - 1] ^= 0xFF;
    }
    return 0;
}

/* Writes part of a file that is there, growing it when the range ends past
 * its end. The part writes between two opens count as one write of the file,
 * so `writes` says how often a file was written whichever way it was. */
static int root_write_at(void *ctx, const char *path, uint64_t offset, const uint8_t *data, size_t len) {
    mem_root *r = (mem_root *)ctx;
    mem_file *f = root_find(r, path);
    if (!r->in_parts++) {
        r->writes++;
        if (r->writes == 1) snprintf(r->first_write, sizeof(r->first_write), "%s", path);
    }
    r->parts++;
    if (f && r->parts == r->fail_part && r->torn) {
        size_t keep = len < 4096 ? len : 4096;
        offset += len - keep;
        data += len - keep;
        len = keep;
    } else if (!f || r->parts == r->fail_part || (r->exclusive && r->open_streams)) {
        return -1;
    }
    if (offset + len > f->len) {
        f->data = (uint8_t *)realloc(f->data, (size_t)(offset + len));
        memset(f->data + f->len, 0, (size_t)(offset + len) - f->len);
        f->len = (size_t)(offset + len);
    }
    memcpy(f->data + offset, data, len);
    if (r->parts == r->corrupt_part && len > 0) f->data[offset + len - 1] ^= 0xFF;
    r->part_bytes += len;
    return r->parts == r->fail_part ? -1 : 0;
}

/* A path ending in '/' is a directory: it goes when no file is left under it. */
static int root_remove(void *ctx, const char *path) {
    mem_root *r = (mem_root *)ctx;
    size_t len = strlen(path);
    if (len && path[len - 1] == '/') {
        for (size_t i = 0; i < r->count; i++) {
            if (strncmp(r->files[i].path, path, len) == 0) return -1;
        }
        r->dirs_removed++;
        return 0;
    }
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

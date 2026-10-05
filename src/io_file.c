// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>

wchar_t *sigil_wide_path(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n) != n) {
        free(wide);
        return NULL;
    }
    return wide;
}

FILE *sigil_fopen(const char *path, const char *mode) {
    wchar_t *wpath = sigil_wide_path(path);
    wchar_t *wmode = sigil_wide_path(mode);
    FILE *fp = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
    free(wpath);
    free(wmode);
    return fp;
}
#else
FILE *sigil_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}
#endif

typedef struct {
    FILE *fp;
    int64_t size;
} file_ctx;

static int file_read(void *ctx, uint64_t off, void *buf, size_t len) {
    file_ctx *fc = (file_ctx *)ctx;
    if (fseeko(fc->fp, (off_t)off, SEEK_SET) != 0) return SIGIL_ERR_IO;
    size_t got = fread(buf, 1, len, fc->fp);
    return (int)got;
}

static int64_t file_size(void *ctx) {
    return ((file_ctx *)ctx)->size;
}

static void file_close(void *ctx) {
    file_ctx *fc = (file_ctx *)ctx;
    if (fc->fp) fclose(fc->fp);
    free(fc);
}

sigil_io *sigil_io_open_file(const char *path) {
    if (!path) return NULL;
    FILE *fp = sigil_fopen(path, "rb");
    if (!fp) return NULL;

    if (fseeko(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    off_t sz = ftello(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);

    file_ctx *fc = (file_ctx *)calloc(1, sizeof(*fc));
    if (!fc) { fclose(fp); return NULL; }
    fc->fp = fp;
    fc->size = (int64_t)sz;

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { fclose(fp); free(fc); return NULL; }
    io->read = file_read;
    io->size = file_size;
    io->close = file_close;
    io->ctx = fc;
    return io;
}

void sigil_io_close(sigil_io *io) {
    if (!io) return;
    if (io->close) io->close(io->ctx);
    free(io);
}

int sigil_io_read_upto(const sigil_io *io, uint64_t off, void *buf, size_t len, size_t *got) {
    if (!io || !io->read || !got) return SIGIL_ERR_INVALID_ARG;
    uint8_t *p = (uint8_t *)buf;
    *got = 0;
    while (*got < len) {
        int n = io->read(io->ctx, off + *got, p + *got, len - *got);
        if (n < 0) return SIGIL_ERR_IO;
        if (n == 0) break;
        *got += (size_t)n;
    }
    return SIGIL_OK;
}

int sigil_io_read_exact(const sigil_io *io, uint64_t off, void *buf, size_t len) {
    if (!io || !io->read) return SIGIL_ERR_INVALID_ARG;
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        int got = io->read(io->ctx, off, p, remaining);
        if (got <= 0) return SIGIL_ERR_IO;
        p += got;
        off += (uint64_t)got;
        remaining -= (size_t)got;
    }
    return SIGIL_OK;
}

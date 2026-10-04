// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t CD_SYNC_PATTERN[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

#define COOKED_SECTOR 2048

typedef struct {
    FILE    *fp;
    uint64_t total_size;
    uint32_t unit_bytes;
    uint32_t data_offset;
} raw_ctx;

static int raw_io_read(void *ctx_, uint64_t off, void *buf, size_t len) {
    raw_ctx *ctx = (raw_ctx *)ctx_;
    uint8_t *out = (uint8_t *)buf;
    size_t total = 0;

    if (ctx->unit_bytes == COOKED_SECTOR && ctx->data_offset == 0) {
        if (fseeko(ctx->fp, (off_t)off, SEEK_SET) != 0) return SIGIL_ERR_IO;
        return (int)fread(out, 1, len, ctx->fp);
    }

    while (len > 0) {
        uint32_t lba = (uint32_t)(off / COOKED_SECTOR);
        size_t intra = (size_t)(off % COOKED_SECTOR);
        uint64_t phys_off = (uint64_t)lba * ctx->unit_bytes + ctx->data_offset;

        uint8_t sector[COOKED_SECTOR];
        if (fseeko(ctx->fp, (off_t)phys_off, SEEK_SET) != 0) {
            if (total > 0) return (int)total;
            return SIGIL_ERR_IO;
        }
        size_t got = fread(sector, 1, COOKED_SECTOR, ctx->fp);
        if (got == 0) {
            if (total > 0) return (int)total;
            return SIGIL_ERR_IO;
        }
        size_t available = got - intra;
        size_t copy = available < len ? available : len;
        memcpy(out, sector + intra, copy);
        out += copy;
        off += copy;
        len -= copy;
        total += copy;
    }
    return (int)total;
}

static int64_t raw_io_size(void *ctx_) {
    raw_ctx *ctx = (raw_ctx *)ctx_;
    if (ctx->unit_bytes == COOKED_SECTOR) return (int64_t)ctx->total_size;
    uint64_t sectors = ctx->total_size / ctx->unit_bytes;
    return (int64_t)(sectors * COOKED_SECTOR);
}

static void raw_io_close(void *ctx_) {
    raw_ctx *ctx = (raw_ctx *)ctx_;
    if (!ctx) return;
    if (ctx->fp) fclose(ctx->fp);
    free(ctx);
}

sigil_io *sigil_io_open_raw_cd(const char *path) {
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseeko(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    off_t sz = ftello(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);

    raw_ctx *ctx = (raw_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) { fclose(fp); return NULL; }
    ctx->fp = fp;
    ctx->total_size = (uint64_t)sz;

    uint8_t probe[2352];
    if (fseeko(fp, 16 * 2352, SEEK_SET) == 0
        && fread(probe, 1, sizeof(probe), fp) == sizeof(probe)
        && memcmp(probe, CD_SYNC_PATTERN, 12) == 0) {
        uint8_t mode = probe[15];
        if (mode == 1) {
            ctx->unit_bytes = 2352;
            ctx->data_offset = 16;
        } else if (mode == 2) {
            ctx->unit_bytes = 2352;
            ctx->data_offset = 24;
        }
    }
    if (ctx->unit_bytes == 0) {
        ctx->unit_bytes = COOKED_SECTOR;
        ctx->data_offset = 0;
    }

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { raw_io_close(ctx); return NULL; }
    io->read = raw_io_read;
    io->size = raw_io_size;
    io->close = raw_io_close;
    io->ctx = ctx;
    return io;
}

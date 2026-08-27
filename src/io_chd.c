// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <libchdr/chd.h>
#include <stdlib.h>
#include <string.h>

#define CHD_SECTOR_SIZE 2048
#define MAX_HUNK_BYTES (16 * 1024 * 1024)

static const uint8_t CD_SYNC_PATTERN[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

typedef struct {
    chd_file *chd;
    uint8_t  *hunk_buffer;
    uint32_t  hunk_bytes;
    uint32_t  unit_bytes;
    uint32_t  frames_per_hunk;
    uint32_t  total_frames;
    uint32_t  data_offset;
    int       last_hunk;
} chd_ctx;

static int chd_read_sector(chd_ctx *ctx, uint32_t lba, uint8_t out[CHD_SECTOR_SIZE]) {
    if (lba >= ctx->total_frames) return SIGIL_ERR_IO;
    int hunk = (int)(lba / ctx->frames_per_hunk);
    int frame_in_hunk = (int)(lba % ctx->frames_per_hunk);

    if (hunk != ctx->last_hunk) {
        chd_error err = chd_read(ctx->chd, hunk, ctx->hunk_buffer);
        if (err != CHDERR_NONE) return SIGIL_ERR_IO;
        ctx->last_hunk = hunk;
    }
    uint8_t *frame = ctx->hunk_buffer + (uint32_t)frame_in_hunk * ctx->unit_bytes;
    memcpy(out, frame + ctx->data_offset, CHD_SECTOR_SIZE);
    return SIGIL_OK;
}

static int chd_io_read(void *ctx_, uint64_t off, void *buf, size_t len) {
    chd_ctx *ctx = (chd_ctx *)ctx_;
    uint8_t *out = (uint8_t *)buf;
    uint8_t sector[CHD_SECTOR_SIZE];
    size_t total = 0;

    while (len > 0) {
        uint32_t lba = (uint32_t)(off / CHD_SECTOR_SIZE);
        size_t intra = (size_t)(off % CHD_SECTOR_SIZE);
        int rc = chd_read_sector(ctx, lba, sector);
        if (rc != SIGIL_OK) {
            if (total > 0) return (int)total;
            return rc;
        }
        size_t copy = CHD_SECTOR_SIZE - intra;
        if (copy > len) copy = len;
        memcpy(out, sector + intra, copy);
        out += copy;
        off += copy;
        len -= copy;
        total += copy;
    }
    return (int)total;
}

static int64_t chd_io_size(void *ctx_) {
    chd_ctx *ctx = (chd_ctx *)ctx_;
    return (int64_t)ctx->total_frames * CHD_SECTOR_SIZE;
}

static void chd_io_close(void *ctx_) {
    chd_ctx *ctx = (chd_ctx *)ctx_;
    if (!ctx) return;
    if (ctx->chd) chd_close(ctx->chd);
    free(ctx->hunk_buffer);
    free(ctx);
}

sigil_io *sigil_io_open_chd(const char *path) {
    if (!path) return NULL;
    chd_file *chd = NULL;
    chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) return NULL;

    const chd_header *header = chd_get_header(chd);
    if (!header || header->unitbytes == 0 || header->hunkbytes == 0
        || header->hunkbytes > MAX_HUNK_BYTES) {
        chd_close(chd);
        return NULL;
    }

    chd_ctx *ctx = (chd_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) { chd_close(chd); return NULL; }
    ctx->hunk_buffer = (uint8_t *)malloc(header->hunkbytes);
    if (!ctx->hunk_buffer) { free(ctx); chd_close(chd); return NULL; }

    ctx->chd = chd;
    ctx->hunk_bytes = header->hunkbytes;
    ctx->unit_bytes = header->unitbytes;
    ctx->frames_per_hunk = ctx->hunk_bytes / ctx->unit_bytes;
    ctx->total_frames = (uint32_t)(header->logicalbytes / ctx->unit_bytes);
    ctx->last_hunk = -1;

    /* CD CHDs use 2448-byte units; user data offset depends on the mode byte
     * at frame[15]: MODE1 = 16, MODE2 = 24. */
    ctx->data_offset = 0;
    if (ctx->unit_bytes > CHD_SECTOR_SIZE) {
        uint32_t probe_lba = 16;
        if (probe_lba < ctx->total_frames) {
            int probe_hunk = (int)(probe_lba / ctx->frames_per_hunk);
            err = chd_read(chd, probe_hunk, ctx->hunk_buffer);
            if (err == CHDERR_NONE) {
                int probe_frame = (int)(probe_lba % ctx->frames_per_hunk);
                uint8_t *frame = ctx->hunk_buffer + (uint32_t)probe_frame * ctx->unit_bytes;
                if (memcmp(frame, CD_SYNC_PATTERN, 12) == 0) {
                    uint8_t mode = frame[15];
                    if (mode == 1) ctx->data_offset = 16;
                    else if (mode == 2) ctx->data_offset = 24;
                }
            }
            ctx->last_hunk = -1;
        }
        if (ctx->data_offset + CHD_SECTOR_SIZE > ctx->unit_bytes) {
            ctx->data_offset = 0;
        }
    }

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { chd_io_close(ctx); return NULL; }
    io->read = chd_io_read;
    io->size = chd_io_size;
    io->close = chd_io_close;
    io->ctx = ctx;
    return io;
}

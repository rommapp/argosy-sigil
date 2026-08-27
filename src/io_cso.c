// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

#define CSO_MAGIC               "CISO"
#define CSO_HEADER_SIZE         24
#define CSO_INDEX_UNCOMPRESSED  0x80000000u
#define CSO_MAX_BLOCK_BYTES     (1u << 17)

typedef struct {
    FILE     *fp;
    uint32_t *index;
    uint64_t  uncompressed_size;
    uint32_t  block_size;
    uint32_t  total_blocks;
    uint32_t  alignment;
    uint8_t  *block_cache;
    uint8_t  *compressed_buf;
    size_t    compressed_buf_cap;
    int64_t   cached_block;
} cso_ctx;

static int cso_read_raw(FILE *fp, uint64_t off, void *buf, size_t len) {
#if defined(_WIN32)
    if (_fseeki64(fp, (long long)off, SEEK_SET) != 0) return -1;
#else
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0) return -1;
#endif
    size_t got = fread(buf, 1, len, fp);
    if (got != len) return -1;
    return 0;
}

static int cso_load_block(cso_ctx *ctx, uint32_t block_idx) {
    if ((int64_t)block_idx == ctx->cached_block) return SIGIL_OK;
    if (block_idx >= ctx->total_blocks) return SIGIL_ERR_IO;

    uint32_t entry      = ctx->index[block_idx];
    uint32_t next_entry = ctx->index[block_idx + 1];
    int      uncompressed = (entry & CSO_INDEX_UNCOMPRESSED) != 0;

    uint64_t file_off    = ((uint64_t)(entry      & ~CSO_INDEX_UNCOMPRESSED)) << ctx->alignment;
    uint64_t next_off    = ((uint64_t)(next_entry & ~CSO_INDEX_UNCOMPRESSED)) << ctx->alignment;
    if (next_off <= file_off) return SIGIL_ERR_IO;
    size_t   stored_size = (size_t)(next_off - file_off);

    if (uncompressed) {
        if (stored_size < ctx->block_size) return SIGIL_ERR_IO;
        if (cso_read_raw(ctx->fp, file_off, ctx->block_cache, ctx->block_size) != 0) {
            return SIGIL_ERR_IO;
        }
        ctx->cached_block = block_idx;
        return SIGIL_OK;
    }

    if (stored_size > ctx->compressed_buf_cap) {
        uint8_t *grow = (uint8_t *)realloc(ctx->compressed_buf, stored_size);
        if (!grow) return SIGIL_ERR_OOM;
        ctx->compressed_buf = grow;
        ctx->compressed_buf_cap = stored_size;
    }
    if (cso_read_raw(ctx->fp, file_off, ctx->compressed_buf, stored_size) != 0) {
        return SIGIL_ERR_IO;
    }

    z_stream strm = {0};
    strm.next_in   = ctx->compressed_buf;
    strm.avail_in  = (uInt)stored_size;
    strm.next_out  = ctx->block_cache;
    strm.avail_out = ctx->block_size;
    if (inflateInit2(&strm, -15) != Z_OK) return SIGIL_ERR_IO;
    int zrc = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (zrc != Z_STREAM_END && zrc != Z_OK) return SIGIL_ERR_IO;
    if (strm.total_out != ctx->block_size) return SIGIL_ERR_IO;

    ctx->cached_block = block_idx;
    return SIGIL_OK;
}

static int cso_io_read(void *ctx_, uint64_t off, void *buf, size_t len) {
    cso_ctx *ctx = (cso_ctx *)ctx_;
    uint8_t *out = (uint8_t *)buf;
    size_t   total = 0;

    if (off >= ctx->uncompressed_size) return 0;
    if (off + len > ctx->uncompressed_size) {
        len = (size_t)(ctx->uncompressed_size - off);
    }

    while (len > 0) {
        uint32_t block_idx = (uint32_t)(off / ctx->block_size);
        size_t   intra     = (size_t)(off % ctx->block_size);
        int rc = cso_load_block(ctx, block_idx);
        if (rc != SIGIL_OK) {
            if (total > 0) return (int)total;
            return rc;
        }
        size_t copy = ctx->block_size - intra;
        if (copy > len) copy = len;
        memcpy(out, ctx->block_cache + intra, copy);
        out   += copy;
        off   += copy;
        len   -= copy;
        total += copy;
    }
    return (int)total;
}

static int64_t cso_io_size(void *ctx_) {
    cso_ctx *ctx = (cso_ctx *)ctx_;
    return (int64_t)ctx->uncompressed_size;
}

static void cso_io_close(void *ctx_) {
    cso_ctx *ctx = (cso_ctx *)ctx_;
    if (!ctx) return;
    if (ctx->fp) fclose(ctx->fp);
    free(ctx->index);
    free(ctx->block_cache);
    free(ctx->compressed_buf);
    free(ctx);
}

sigil_io *sigil_io_open_cso(const char *path) {
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    uint8_t hdr[CSO_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) { fclose(fp); return NULL; }
    if (memcmp(hdr, CSO_MAGIC, 4) != 0) { fclose(fp); return NULL; }

    uint32_t header_size       = sigil_read_le32(hdr + 4);
    uint64_t uncompressed_size = sigil_read_le64(hdr + 8);
    uint32_t block_size        = sigil_read_le32(hdr + 16);
    uint8_t  version           = hdr[20];
    uint8_t  alignment         = hdr[21];

    if (header_size == 0) header_size = CSO_HEADER_SIZE;
    if (version != 1) { fclose(fp); return NULL; }
    if (block_size == 0 || block_size > CSO_MAX_BLOCK_BYTES) { fclose(fp); return NULL; }
    if (uncompressed_size == 0) { fclose(fp); return NULL; }

    uint64_t total_blocks_64 = uncompressed_size / block_size;
    if (uncompressed_size % block_size != 0) total_blocks_64++;
    if (total_blocks_64 > UINT32_MAX - 1) { fclose(fp); return NULL; }
    uint32_t total_blocks = (uint32_t)total_blocks_64;

    cso_ctx *ctx = (cso_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) { fclose(fp); return NULL; }
    ctx->fp                 = fp;
    ctx->uncompressed_size  = uncompressed_size;
    ctx->block_size         = block_size;
    ctx->total_blocks       = total_blocks;
    ctx->alignment          = alignment;
    ctx->cached_block       = -1;

    size_t index_bytes = (size_t)(total_blocks + 1) * sizeof(uint32_t);
    ctx->index = (uint32_t *)malloc(index_bytes);
    ctx->block_cache = (uint8_t *)malloc(block_size);
    if (!ctx->index || !ctx->block_cache) { cso_io_close(ctx); return NULL; }

    if (cso_read_raw(fp, header_size, ctx->index, index_bytes) != 0) {
        cso_io_close(ctx);
        return NULL;
    }

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { cso_io_close(ctx); return NULL; }
    io->read  = cso_io_read;
    io->size  = cso_io_size;
    io->close = cso_io_close;
    io->ctx   = ctx;
    return io;
}

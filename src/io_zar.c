// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

#if SIGIL_WITH_ZARCHIVE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zstd.h>

/* Presents one file inside a ZArchive as a normal sigil_io.
 *
 * Unlike the zip shim this is genuine random access, because ZArchive stores
 * contents in fixed 64 KiB blocks and keeps an offset record for every 16 of
 * them. Reaching an arbitrary byte costs one record read, one compressed block
 * read and one zstd call, no matter how far into the archive it sits. That is
 * the whole reason Cemu can stream a game out of a .wua while it runs, and it
 * means identifying a title costs a couple of blocks rather than a full
 * decompression. */

typedef struct {
    sigil_io        *backing;      /* owns the underlying file IO */
    sigil_zar_footer footer;

    uint64_t         file_off;     /* member start, in the uncompressed space */
    uint64_t         file_size;

    uint64_t         block_count;
    uint8_t         *block;        /* SIGIL_ZAR_BLOCK_SIZE decoded bytes */
    uint8_t         *comp;         /* compressed input staging */
    int64_t          cached_block;
} zar_ctx;

/* Compressed position of a block: the record holds a full 64-bit base for its
 * first block, and every stored size is one less than the real one, so the
 * blocks in between are reached by summing forward. */
static int zar_block_location(zar_ctx *c, uint64_t block_idx,
                              uint64_t *out_off, uint32_t *out_size) {
    uint64_t record = block_idx / SIGIL_ZAR_BLOCKS_PER_RECORD;
    uint32_t sub    = (uint32_t)(block_idx % SIGIL_ZAR_BLOCKS_PER_RECORD);

    uint64_t record_off = c->footer.offset_records_off
                        + record * SIGIL_ZAR_RECORD_SIZE;
    if (record_off + SIGIL_ZAR_RECORD_SIZE
        > c->footer.offset_records_off + c->footer.offset_records_size) {
        return SIGIL_ERR_NOT_FOUND;
    }

    uint8_t rec[SIGIL_ZAR_RECORD_SIZE];
    if (sigil_io_read_exact(c->backing, record_off, rec, sizeof(rec)) != SIGIL_OK) {
        return SIGIL_ERR_IO;
    }

    uint64_t off = sigil_read_be64(rec);
    for (uint32_t i = 0; i < sub; i++) {
        off += (uint64_t)((rec[8 + i * 2] << 8) | rec[9 + i * 2]) + 1;
    }
    uint32_t size = (uint32_t)((rec[8 + sub * 2] << 8) | rec[9 + sub * 2]) + 1;

    if (off + size > c->footer.compressed_size) return SIGIL_ERR_NOT_FOUND;
    *out_off  = c->footer.compressed_off + off;
    *out_size = size;
    return SIGIL_OK;
}

/* A block whose compressed size equals the block size was stored raw, which is
 * what the writer does when compression would not have paid for itself. */
static int zar_load_block(zar_ctx *c, uint64_t block_idx) {
    if (c->cached_block == (int64_t)block_idx) return SIGIL_OK;
    if (block_idx >= c->block_count) return SIGIL_ERR_NOT_FOUND;

    uint64_t off;
    uint32_t size;
    int rc = zar_block_location(c, block_idx, &off, &size);
    if (rc != SIGIL_OK) return rc;

    if (size == SIGIL_ZAR_BLOCK_SIZE) {
        if (sigil_io_read_exact(c->backing, off, c->block, size) != SIGIL_OK) {
            return SIGIL_ERR_IO;
        }
    } else {
        if (sigil_io_read_exact(c->backing, off, c->comp, size) != SIGIL_OK) {
            return SIGIL_ERR_IO;
        }
        size_t got = ZSTD_decompress(c->block, SIGIL_ZAR_BLOCK_SIZE, c->comp, size);
        if (ZSTD_isError(got) || got != SIGIL_ZAR_BLOCK_SIZE) return SIGIL_ERR_NOT_FOUND;
    }

    c->cached_block = (int64_t)block_idx;
    return SIGIL_OK;
}

static int zar_io_read(void *ctx, uint64_t off, void *buf, size_t len) {
    zar_ctx *c = (zar_ctx *)ctx;
    if (off >= c->file_size) return 0;

    uint64_t avail = c->file_size - off;
    if (len > avail) len = (size_t)avail;

    size_t done = 0;
    while (done < len) {
        uint64_t raw   = c->file_off + off + done;
        uint64_t block = raw / SIGIL_ZAR_BLOCK_SIZE;
        uint32_t within = (uint32_t)(raw % SIGIL_ZAR_BLOCK_SIZE);

        if (zar_load_block(c, block) != SIGIL_OK) break;

        size_t step = SIGIL_ZAR_BLOCK_SIZE - within;
        if (step > len - done) step = len - done;
        memcpy((uint8_t *)buf + done, c->block + within, step);
        done += step;
    }
    return (int)done;
}

static int64_t zar_io_size(void *ctx) {
    return (int64_t)((zar_ctx *)ctx)->file_size;
}

static void zar_io_close(void *ctx) {
    zar_ctx *c = (zar_ctx *)ctx;
    if (!c) return;
    if (c->backing) sigil_io_close(c->backing);
    free(c->block);
    free(c->comp);
    free(c);
}

/* Finds `name` among the root directory's children. Xenia mounts a .zar at "/"
 * and expects default.xex there, so a single level is all that is needed. */
static int zar_find_root_file(const sigil_io *io, const sigil_zar_footer *ft,
                              const char *name,
                              uint64_t *out_off, uint64_t *out_size) {
    if (ft->names_size > SIGIL_ZAR_MAX_METADATA
        || ft->file_tree_size > SIGIL_ZAR_MAX_METADATA
        || ft->file_tree_size < SIGIL_ZAR_TREE_ENTRY) {
        return SIGIL_ERR_NOT_FOUND;
    }

    uint8_t *names = (uint8_t *)malloc((size_t)ft->names_size);
    uint8_t *tree  = (uint8_t *)malloc((size_t)ft->file_tree_size);
    if (!names || !tree) { free(names); free(tree); return SIGIL_ERR_OOM; }

    int rc = SIGIL_ERR_NOT_FOUND;
    if (sigil_io_read_exact(io, ft->names_off, names, (size_t)ft->names_size) != SIGIL_OK
        || sigil_io_read_exact(io, ft->file_tree_off, tree, (size_t)ft->file_tree_size) != SIGIL_OK) {
        free(names); free(tree);
        return SIGIL_ERR_IO;
    }

    uint32_t root_flags = sigil_read_be32(tree);
    if (root_flags & 0x80000000u) { free(names); free(tree); return SIGIL_ERR_NOT_FOUND; }

    uint32_t start = sigil_read_be32(tree + 4);
    uint32_t count = sigil_read_be32(tree + 8);
    uint32_t total = (uint32_t)(ft->file_tree_size / SIGIL_ZAR_TREE_ENTRY);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = start + i;
        if (idx >= total) break;

        const uint8_t *e = tree + (size_t)idx * SIGIL_ZAR_TREE_ENTRY;
        uint32_t nf = sigil_read_be32(e);
        if ((nf & 0x80000000u) == 0) continue;   /* directory */

        char entry_name[256];
        if (sigil_zar_read_name(names, (size_t)ft->names_size, nf & 0x7FFFFFFFu,
                                entry_name, sizeof(entry_name)) == 0) {
            continue;
        }
        if (strcasecmp(entry_name, name) != 0) continue;

        uint32_t off_low  = sigil_read_be32(e + 4);
        uint32_t size_low = sigil_read_be32(e + 8);
        uint32_t high     = sigil_read_be32(e + 12);
        *out_off  = (uint64_t)off_low  | ((uint64_t)(high & 0xFFFFu) << 32);
        *out_size = (uint64_t)size_low | ((uint64_t)(high & 0xFFFF0000u) << 16);
        rc = SIGIL_OK;
        break;
    }

    free(names);
    free(tree);
    return rc;
}

sigil_io *sigil_io_open_zar(const char *path, const char *member) {
    if (!path || !member) return NULL;

    sigil_io *backing = sigil_io_open_file(path);
    if (!backing) return NULL;

    sigil_zar_footer ft;
    if (sigil_zar_read_footer(backing, &ft) != SIGIL_OK) {
        sigil_io_close(backing);
        return NULL;
    }

    uint64_t file_off, file_size;
    if (zar_find_root_file(backing, &ft, member, &file_off, &file_size) != SIGIL_OK) {
        sigil_io_close(backing);
        return NULL;
    }

    zar_ctx *c = (zar_ctx *)calloc(1, sizeof(*c));
    if (!c) { sigil_io_close(backing); return NULL; }
    c->backing      = backing;
    c->footer       = ft;
    c->file_off     = file_off;
    c->file_size    = file_size;
    c->block_count  = ft.offset_records_size / SIGIL_ZAR_RECORD_SIZE
                    * SIGIL_ZAR_BLOCKS_PER_RECORD;
    c->cached_block = -1;
    c->block = (uint8_t *)malloc(SIGIL_ZAR_BLOCK_SIZE);
    c->comp  = (uint8_t *)malloc(SIGIL_ZAR_BLOCK_SIZE);
    if (!c->block || !c->comp) { zar_io_close(c); return NULL; }

    sigil_io *io = (sigil_io *)calloc(1, sizeof(*io));
    if (!io) { zar_io_close(c); return NULL; }
    io->read  = zar_io_read;
    io->size  = zar_io_size;
    io->close = zar_io_close;
    io->ctx   = c;
    return io;
}

#else

sigil_io *sigil_io_open_zar(const char *path, const char *member) {
    (void)path; (void)member;
    return NULL;
}

#endif

// SPDX-License-Identifier: MPL-2.0
#include "card_ps1.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *data; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }
static void mem_close(void *ctx) { free(ctx); }

/* A save root of one file, the game's card, which the input may replace. */
typedef struct {
    uint8_t *card;
    size_t   len;
} root;

static sigil_io *root_open(void *ctx, const char *path) {
    root *r = (root *)ctx;
    if (strcmp(path, "Game.srm") != 0 || !r->card) return NULL;
    mem_ctx *m = (mem_ctx *)malloc(sizeof(*m));
    sigil_io *io = (sigil_io *)malloc(sizeof(*io));
    if (!m || !io) { free(m); free(io); return NULL; }
    m->data = r->card;
    m->len = r->len;
    io->read = mem_read;
    io->size = mem_size;
    io->close = mem_close;
    io->ctx = m;
    return io;
}

static int root_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    root *r = (root *)ctx;
    if (strcmp(path, "Game.srm") != 0) return -1;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return -1;
    memcpy(copy, data, len);
    free(r->card);
    r->card = copy;
    r->len = len;
    return 0;
}

/* Restores the input as a unit from RomM, and as a companion's unit, into a
 * root whose card holds the input too, then collects the root and restores
 * the collected units. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const char *const listing[] = { "Game.srm" };
    static const char *const ids[] = { "SLUS-01334" };
    static const char *const companion_ids[] = { "SLUS-00453" };
    root r = { (uint8_t *)malloc(size ? size : 1), size };
    if (!r.card) return 0;
    if (size) memcpy(r.card, data, size);

    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = SIGIL_RESULT_V3;
    result.platform = SIGIL_PLATFORM_PSX;
    strcpy(result.title_id, "SLUS-01334");

    sigil_sync_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SYNC_REQUEST_V1;
    req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.save.layout = "pcsx_rearmed";
    req.save.platform = "psx";
    req.save.content_path = "Game.cue";
    req.save.result = &result;
    req.save.listing = listing;
    req.save.listing_count = 1;
    req.save.open = root_open;
    req.save.open_ctx = &r;
    req.game_ids = ids;
    req.game_id_count = 1;
    req.overwrite_local = 1;
    req.write = root_write;
    req.write_ctx = &r;
    sigil_sync_companion companion = { companion_ids, 1, data, size };
    req.companions = &companion;
    req.companion_count = 1;

    sigil_sync_result *out = NULL;
    sigil_restore(&req, data, size, &out);
    sigil_sync_result_free(out);
    out = NULL;
    companion.unit = NULL;
    companion.unit_len = 0;
    if (sigil_collect(&req, &out) == SIGIL_OK && out->data) {
        if (out->companion_count == 1 && out->companions[0].data) {
            companion.unit = out->companions[0].data;
            companion.unit_len = out->companions[0].len;
        }
        sigil_sync_result *again = NULL;
        sigil_restore(&req, out->data, out->len, &again);
        sigil_sync_result_free(again);
    }
    sigil_sync_result_free(out);
    free(r.card);
    return 0;
}

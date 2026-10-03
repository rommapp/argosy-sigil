// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdio.h>
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

/* A save root of up to ROOT_FILES files, starting from two named ones: the
 * per-game internal and cart volumes, VMU A1 and B1, or two .gci files.
 * Writes may add files and removes take them away. */
#define ROOT_FILES 8

typedef struct {
    uint8_t    *file[ROOT_FILES];
    size_t      len[ROOT_FILES];
    char        names[ROOT_FILES][SIGIL_SAVE_PATH_MAX];
    const char *listing[ROOT_FILES];
    size_t      count;
} root;

static int slot_in(const root *r, const char *path) {
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(path, r->names[i]) == 0) return (int)i;
    }
    return -1;
}

static sigil_io *root_open(void *ctx, const char *path) {
    root *r = (root *)ctx;
    int i = slot_in(r, path);
    if (i < 0 || !r->file[i]) return NULL;
    mem_ctx *m = (mem_ctx *)malloc(sizeof(*m));
    sigil_io *io = (sigil_io *)malloc(sizeof(*io));
    if (!m || !io) { free(m); free(io); return NULL; }
    m->data = r->file[i];
    m->len = r->len[i];
    io->read = mem_read;
    io->size = mem_size;
    io->close = mem_close;
    io->ctx = m;
    return io;
}

static int root_write(void *ctx, const char *path, const uint8_t *data, size_t len) {
    root *r = (root *)ctx;
    int i = slot_in(r, path);
    if (i < 0) {
        if (r->count >= ROOT_FILES) return -1;
        i = (int)r->count++;
        snprintf(r->names[i], sizeof(r->names[i]), "%s", path);
        r->listing[i] = r->names[i];
    }
    uint8_t *copy = (uint8_t *)malloc(len ? len : 1);
    if (!copy) return -1;
    memcpy(copy, data, len);
    free(r->file[i]);
    r->file[i] = copy;
    r->len[i] = len;
    return 0;
}

static int root_remove(void *ctx, const char *path) {
    root *r = (root *)ctx;
    int i = slot_in(r, path);
    if (i < 0) return -1;
    free(r->file[i]);
    r->file[i] = NULL;
    r->len[i] = 0;
    return 0;
}

static void broken(const char *what) {
    fprintf(stderr, "oracle: %s\n", what);
    abort();
}

typedef struct {
    const char              *layout;
    const char              *platform;
    const char              *content;
    int                      result_platform;   /* 0 for no sigil_result */
    const char              *title_id;
    const char              *serial;
    const char              *paths[2];
    const sigil_save_option *options;
    size_t                   option_count;
} sync_mode;

static const sigil_save_option SEGACD_PER_GAME[] = {
    { "genesis_plus_gx_system_bram", "per game" },
    { "genesis_plus_gx_cart_bram", "per game" },
};
static const sigil_save_option VMU_ALL[] = { { "reicast_per_content_vmus", "All VMUs" } };
static const sigil_save_option GC_RAW[] = { { "SlotA", "1" }, { "MemoryCardSize", "-1" } };
static const sigil_save_option SATURN_SHARED[] = {
    { "beetle_saturn_save_method", "mednafen" },
    { "beetle_saturn_shared_int", "enabled" },
    { "beetle_saturn_shared_ext", "enabled" },
};

static const sync_mode MODES[] = {
    { "genesis_plus_gx", "segacd", "Game.cue", 0, "", "", { "Game.brm", "Game_4Mbit_cart.brm" }, SEGACD_PER_GAME, 2 },
    { "flycast", "dreamcast", "Game.gdi", SIGIL_PLATFORM_DREAMCAST, "T-1", "", { "T-1.A1.bin", "T-1.B1.bin" }, VMU_ALL, 1 },
    { "dolphin", "gamecube", "Game.iso", SIGIL_PLATFORM_GAMECUBE, "47465A45", "GFZE",
      { "User/GC/USA/Card A/a.gci", "User/GC/USA/Card A/b.gci" }, NULL, 0 },
    { "dolphin", "gamecube", "Game.iso", SIGIL_PLATFORM_GAMECUBE, "47465A45", "GFZE",
      { "User/GC/MemoryCardA.USA.raw", "User/GC/MemoryCardA.EUR.raw" }, GC_RAW, 2 },
    { "mednafen_saturn", "saturn", "Game.cue", 0, "", "", { "Game.srm", "Game.bcr" }, NULL, 0 },
    { "genesis_plus_gx", "segacd", "Game (USA).cue", 0, "", "", { "scd_U.brm", "4Mbit_cart.brm" }, NULL, 0 },
    { "mednafen_saturn", "saturn", "Game.cue", 0, "", "",
      { "mednafen_saturn_libretro_shared.bkr", "mednafen_saturn_libretro_shared.bcr" }, SATURN_SHARED, 3 },
};
#define MODE_COUNT (sizeof(MODES) / sizeof(MODES[0]))

/* Restores the input as a unit from RomM into an empty root under mode `m`,
 * collects the root and restores the collected unit. A restore that succeeds
 * must collect back to the unit's identity, unchanged. */
static void run_mode(const sync_mode *m, const uint8_t *data, size_t size) {
    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = SIGIL_RESULT_V3;
    result.platform = m->result_platform;
    snprintf(result.title_id, sizeof(result.title_id), "%s", m->title_id);
    snprintf(result.raw_serial, sizeof(result.raw_serial), "%s", m->serial);

    root r;
    memset(&r, 0, sizeof(r));
    for (size_t i = 0; i < 2; i++) {
        snprintf(r.names[i], sizeof(r.names[i]), "%s", m->paths[i]);
        r.listing[i] = r.names[i];
    }
    r.count = 2;
    sigil_sync_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SYNC_REQUEST_V1;
    req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.save.layout = m->layout;
    req.save.platform = m->platform;
    req.save.result = m->result_platform ? &result : NULL;
    req.save.content_path = m->content;
    req.save.listing = r.listing;
    req.save.listing_count = r.count;
    req.save.options = m->options;
    req.save.option_count = m->option_count;
    req.save.open = root_open;
    req.save.open_ctx = &r;
    req.overwrite_local = 1;
    req.write = root_write;
    req.remove = root_remove;
    req.write_ctx = &r;

    sigil_sync_result *out = NULL;
    sigil_restore(&req, data, size, &out);
    sigil_sync_result_free(out);
    out = NULL;
    req.save.listing_count = r.count;
    if (sigil_collect(&req, &out) == SIGIL_OK && out->data) {
        sigil_sync_result *again = NULL, *back = NULL;
        if (sigil_restore(&req, out->data, out->len, &again) == SIGIL_OK) {
            req.save.listing_count = r.count;
            req.state = again->state;
            req.state_len = again->state_len;
            if (sigil_collect(&req, &back) != SIGIL_OK) broken("collect after a restore failed");
            if (strcmp(back->identity_hash, out->identity_hash) != 0) broken("a restored unit collects back as other saves");
            if (back->changed) broken("a restored unit collects back as changed");
        }
        sigil_sync_result_free(back);
        sigil_sync_result_free(again);
    }
    sigil_sync_result_free(out);
    for (size_t i = 0; i < r.count; i++) free(r.file[i]);
}

/* Reads the input as a zip, then runs it through every mode: Sega CD and
 * Saturn on per-game and on shared volumes, a Dreamcast under flycast with
 * every VMU per game, and a GameCube under Dolphin's GCI folder and on its
 * raw card. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sigil_zip_member *members = NULL;
    size_t count = 0;
    if (sigil_zip_read_mem(data, size, 1u << 20, &members, &count) == SIGIL_OK) sigil_zip_members_free(members, count);
    for (size_t i = 0; i < MODE_COUNT; i++) run_mode(&MODES[i], data, size);
    return 0;
}

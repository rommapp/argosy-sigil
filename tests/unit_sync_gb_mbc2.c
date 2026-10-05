// SPDX-License-Identifier: MPL-2.0
/* MBC2 RAM through collect and restore: mGBA's 256 packed bytes, the 512
 * one-cell-per-byte form (with whatever each emulator puts above the cell),
 * and gambatte's 8 KiB span all give one unit, and restore writes each
 * core's own form. */
#include "legacy_units.h"
#include <stdio.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, uint32_t features) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_GB;
    g->result.features = features;
    g->ids[0] = "FFL";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "gb";
    g->req.save.content_path = "FFL.gb";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = g->ids;
    g->req.game_id_count = 1;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
    g->req.overwrite_local = 1;
}

static uint8_t g_cells[512];   /* the neutral form: each cell in the low nibble, 0xF above */
static uint8_t g_packed[256];  /* mGBA: the even cell in the low nibble */

static void make_forms(void) {
    for (size_t i = 0; i < 512; i++) g_cells[i] = (uint8_t)(0xF0 | ((i * 7 + i / 16) & 0x0F));
    for (size_t i = 0; i < 256; i++) g_packed[i] = (uint8_t)((g_cells[2 * i] & 0x0F) | (g_cells[2 * i + 1] & 0x0F) << 4);
}

/* The 512-byte form with `high` above each cell. */
static void cells_with(uint8_t high, uint8_t out[512]) {
    for (size_t i = 0; i < 512; i++) out[i] = (uint8_t)(high << 4 | (g_cells[i] & 0x0F));
}

static sigil_sync_result *collect(mem_root *root, const char *layout, uint32_t features) {
    game g;
    make_game(&g, root, layout, features);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

typedef struct {
    const char *layout;
    const char *file;
    size_t      len;
    uint8_t     high;   /* the 512-byte forms */
} form_case;

/* Every form collects to the neutral cells. */
static void check_collect(void) {
    static const form_case CASES[] = {
        { "mgba", "FFL.srm", 256, 0 },
        { "mgba_standalone", "FFL.sav", 256, 0 },
        { "vbam_standalone", "FFL.sav", 512, 0xF },
        { "gearboy_standalone", "FFL.sav", 512, 0x0 },
        { "sameboy_standalone", "FFL.sav", 512, 0x3 },
        { "gambatte", "FFL.srm", 8192, 0x5 },
        { "tgbdual", "FFL.srm", 8192, 0xA },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const form_case *c = &CASES[i];
        uint8_t file[8192];
        memset(file, 0x00, sizeof(file));
        if (c->len == 256) memcpy(file, g_packed, 256);
        else cells_with(c->high, file);
        mem_root root = {0};
        root_put(&root, c->file, file, c->len);
        sigil_sync_result *r = collect(&root, c->layout, SIGIL_FEATURE_MBC2);
        if (!r || !r->data || r->len != 512 || memcmp(r->data, g_cells, 512) != 0) fail(c->layout, "collect isn't the neutral cells");
        sigil_sync_result_free(r);
        root_free(&root);
    }

    /* An 8 KiB RAM is MBC2's only when the header says so. */
    uint8_t span[8192];
    memset(span, 0x00, sizeof(span));
    cells_with(0x5, span);
    mem_root root = {0};
    root_put(&root, "FFL.srm", span, sizeof(span));
    sigil_sync_result *r = collect(&root, "gambatte", 0);
    if (!r || r->len != 8192 || memcmp(r->data, span, 8192) != 0) fail("8 KiB without the header bit", "converted");
    sigil_sync_result_free(r);
    root_free(&root);
}

/* Restore writes each core's form; a core sigil doesn't know keeps the form already on disk. */
static void check_restore(void) {
    static const struct { const char *layout; const char *file; size_t len; } TARGETS[] = {
        { "mgba", "FFL.srm", 256 },
        { "mgba_standalone", "FFL.sav", 256 },
        { "gambatte", "FFL.srm", 8192 },
        { "vbam_standalone", "FFL.sav", 512 },
        { "sameboy", "FFL.srm", 512 },
    };
    for (size_t i = 0; i < sizeof(TARGETS) / sizeof(TARGETS[0]); i++) {
        mem_root root = {0};
        game g;
        make_game(&g, &root, TARGETS[i].layout, SIGIL_FEATURE_MBC2);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        bool ok = sigil_restore(&g.req, g_cells, sizeof(g_cells), &w) == SIGIL_OK &&
                  (f = root_find(&root, TARGETS[i].file)) && f->len == TARGETS[i].len;
        if (ok && f->len == 256) ok = memcmp(f->data, g_packed, 256) == 0;
        if (ok && f->len >= 512) ok = memcmp(f->data, g_cells, 512) == 0;
        if (ok && f->len == 8192) ok = f->data[512] == 0xFF && f->data[8191] == 0xFF;
        if (!ok) fail(TARGETS[i].layout, "restore didn't write the core's form");
        sigil_sync_result_free(w);
        root_free(&root);
    }

    /* mGBA's packed save into VBA-M: 512 bytes, so VBA-M keeps saving. */
    mem_root vbam = {0};
    game g;
    make_game(&g, &vbam, "vbam_standalone", SIGIL_FEATURE_MBC2);
    sigil_sync_result *w = NULL;
    mem_file *f = NULL;
    if (sigil_restore(&g.req, g_packed, sizeof(g_packed), &w) != SIGIL_OK || !(f = root_find(&vbam, "FFL.sav")) ||
        f->len != 512 || memcmp(f->data, g_cells, 512) != 0) {
        fail("mgba save into vba-m", "not the 512 cells");
    }
    sigil_sync_result_free(w);
    root_free(&vbam);

    mem_root unknown = {0};
    uint8_t blank[256];
    memset(blank, 0xFF, sizeof(blank));
    root_put(&unknown, "FFL.srm", blank, sizeof(blank));
    make_game(&g, &unknown, "libretro", SIGIL_FEATURE_MBC2);
    w = NULL;
    f = NULL;
    if (sigil_restore(&g.req, g_cells, sizeof(g_cells), &w) != SIGIL_OK || !(f = root_find(&unknown, "FFL.srm")) ||
        f->len != 256 || memcmp(f->data, g_packed, 256) != 0) {
        fail("unknown core", "the packed form on disk isn't kept");
    }
    sigil_sync_result_free(w);
    root_free(&unknown);
}

int main(void) {
    make_forms();
    check_collect();
    check_restore();
    printf("gb mbc2 sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}

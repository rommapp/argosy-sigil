// SPDX-License-Identifier: MPL-2.0
/* What the cartridge converters share (sync_cart.c and the suffix tests in
 * sigil_util.h), through collect and restore: each converter names its unit
 * for the content with its own extension, the file is the row's primary
 * member, an empty file holds no save, and N64 takes an emulator file's
 * extension in either case. */
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

static void make_game(game *g, mem_root *root, const char *platform, const char *layout, const char *content,
                      uint32_t features) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = sigil_platform_from_slug(platform);
    g->result.features = features;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", "NXXE");
    snprintf(g->result.n64_header, sizeof(g->result.n64_header), "%s", "GAME");
    g->ids[0] = "NXXE";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
    g->req.save.content_path = content;
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

static sigil_sync_result *collect(mem_root *root, const char *platform, const char *layout, const char *content,
                                  uint32_t features) {
    game g;
    make_game(&g, root, platform, layout, content, features);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static int restore(mem_root *root, const char *platform, const char *layout, const char *content, const uint8_t *unit,
                   size_t len) {
    game g;
    make_game(&g, root, platform, layout, content, 0);
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc;
}

/* Each converter's unit is named for the content: GB's RAM .srm and a clock
 * cart's .zip, N64's .zip, DS's and the Sega carts' .srm. */
static void check_names(void) {
    static const struct {
        const char *platform, *layout, *content, *file;
        size_t      len;
        uint32_t    features;
        const char *artifact;
    } CASES[] = {
        { "gb", "gambatte", "Game.gb", "Game.srm", 0x2000, 0, "Game.srm" },
        { "gb", "gambatte", "Game.gb", "Game.srm", 0x2000, SIGIL_FEATURE_RTC, "Game.zip" },
        { "n64", "mupen64plus_next", "Game (USA).z64", "Game (USA).srm", 0x48800, 0, "Game (USA).zip" },
        { "nds", "melonds", "Game.nds", "Game.sav", 0x10000, 0, "Game.srm" },
        { "genesis", "genesis_plus_gx", "Game.md", "Game.srm", 0x400, 0, "Game.srm" },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        uint8_t *data = (uint8_t *)malloc(CASES[i].len);
        memset(data, 0x22, CASES[i].len);
        mem_root root = {0};
        root_put(&root, CASES[i].file, data, CASES[i].len);
        sigil_sync_result *r = collect(&root, CASES[i].platform, CASES[i].layout, CASES[i].content, CASES[i].features);
        if (!r || !r->data || strcmp(r->artifact, CASES[i].artifact) != 0) {
            fail(CASES[i].artifact, "the unit is named for the content with the converter's extension");
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(data);
    }
}

/* An empty file, or a DeSmuME .dsv of its footer alone, holds no save. */
static void check_empty(void) {
    static const char NOTE[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
    static const char COOKIE[] = "|-DESMUME SAVE-|";
    uint8_t footer[sizeof(NOTE) - 1 + 24 + sizeof(COOKIE) - 1];
    memset(footer, 0, sizeof(footer));
    memcpy(footer, NOTE, sizeof(NOTE) - 1);
    memcpy(footer + sizeof(NOTE) - 1 + 24, COOKIE, sizeof(COOKIE) - 1);
    static const struct { const char *platform, *layout, *content, *file; bool footer; } CASES[] = {
        { "nds", "melonds", "Game.nds", "Game.sav", false },
        { "nds", "desmume", "Game.nds", "Game.dsv", true },
        { "genesis", "picodrive", "Game.md", "Game.srm", false },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        mem_root root = {0};
        if (CASES[i].footer) root_put(&root, CASES[i].file, footer, sizeof(footer));
        else root_put(&root, CASES[i].file, (const uint8_t *)"", 0);
        sigil_sync_result *r = collect(&root, CASES[i].platform, CASES[i].layout, CASES[i].content, 0);
        if (!r || r->data || r->identity_hash[0]) fail(CASES[i].file, "holds no save");
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

/* N64 takes an emulator file's extension in either case; another extension is refused. */
static void check_n64_case(void) {
    static const char *const NAMES[][1] = { { "GAME.EEP" }, { "game.eep" }, { "GAME.TXT" } };
    uint8_t eep[0x200];
    memset(eep, 0x33, sizeof(eep));
    uint8_t *data[1] = { eep };
    size_t lens[1] = { sizeof(eep) };
    for (size_t i = 0; i < 3; i++) {
        size_t zip_len = 0;
        uint8_t *zip = legacy_zip(NAMES[i], data, lens, 1, &zip_len);
        mem_root root = {0};
        int want = i < 2 ? SIGIL_OK : SIGIL_ERR_UNSUPPORTED_FORMAT;
        if (restore(&root, "n64", "mupen64plus_next", "Game (USA).z64", zip, zip_len) != want) {
            fail(NAMES[i][0], i < 2 ? "an EEPROM file restores whatever the case" : "refused");
        }
        root_free(&root);
        free(zip);
    }
}

/* The file is the row's primary member, never a sidecar: on the fbneo row,
 * which has no platform and so serves these carts too, collect takes no save
 * from fbneo/{romset}.nv alone, and restore writes fbneo/{romset}.fs. */
static void check_primary_member(void) {
    static const struct { const char *platform, *content, *primary, *sidecar; } CASES[] = {
        { "genesis", "Game.md", "fbneo/Game.fs", "fbneo/Game.nv" },
        { "nds", "Game.nds", "fbneo/Game.fs", "fbneo/Game.nv" },
    };
    uint8_t side[0x40], save[0x2000];
    memset(side, 0x11, sizeof(side));
    memset(save, 0x22, sizeof(save));
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        mem_root root = {0};
        root_put(&root, CASES[i].sidecar, side, sizeof(side));
        sigil_sync_result *r = collect(&root, CASES[i].platform, "fbneo", CASES[i].content, 0);
        if (!r || r->data) fail(CASES[i].platform, "collect takes no save from the sidecar alone");
        sigil_sync_result_free(r);
        mem_file *p = NULL, *n = NULL;
        if (restore(&root, CASES[i].platform, "fbneo", CASES[i].content, save, sizeof(save)) != SIGIL_OK ||
            !(p = root_find(&root, CASES[i].primary)) || p->len != sizeof(save) || memcmp(p->data, save, sizeof(save)) ||
            !(n = root_find(&root, CASES[i].sidecar)) || n->len != sizeof(side) || memcmp(n->data, side, sizeof(side))) {
            fail(CASES[i].platform, "restore writes the primary file and leaves the sidecar as it was");
        }
        root_free(&root);
    }
}

static void check_suffixes(void) {
    if (!sigil_ends_with("Game.srm", ".srm") || sigil_ends_with("Game.SRM", ".srm") || sigil_ends_with("srm", ".srm") ||
        sigil_ends_with(NULL, ".srm")) {
        fail("sigil_ends_with", "byte for byte, false for a shorter or NULL name");
    }
    if (!sigil_ends_with_nocase("Game.XISO.ISO", ".xiso.iso") || sigil_ends_with_nocase("Game.iso", ".xiso.iso") ||
        sigil_ends_with_nocase(NULL, ".iso")) {
        fail("sigil_ends_with_nocase", "letters without case, false for a shorter or NULL name");
    }
}

int main(void) {
    check_names();
    check_empty();
    check_n64_case();
    check_primary_member();
    check_suffixes();
    printf("cart sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}

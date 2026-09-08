// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "save_unit_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Save-unit resolution over listings, and hashes checked against values the
 * RomM server produced (save_unit_fixture.h). Every layout row gets a
 * listing that exercises its conditions. */

typedef struct { const uint8_t *buf; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}
static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }
static void mem_close(void *ctx) { free(ctx); }

typedef struct {
    const char    *path;
    const uint8_t *data;
    size_t         len;
} fake_file;

typedef struct {
    const fake_file *files;
    size_t           count;
} fake_root;

static sigil_io *fake_open(void *ctx, const char *relative_path) {
    fake_root *root = (fake_root *)ctx;
    for (size_t i = 0; i < root->count; i++) {
        if (strcmp(root->files[i].path, relative_path) != 0) continue;
        mem_ctx *m = (mem_ctx *)malloc(sizeof(*m));
        sigil_io *io = (sigil_io *)malloc(sizeof(*io));
        if (!m || !io) { free(m); free(io); return NULL; }
        m->buf = root->files[i].data;
        m->len = root->files[i].len;
        io->read = mem_read;
        io->size = mem_size;
        io->close = mem_close;
        io->ctx = m;
        return io;
    }
    return NULL;
}

static int g_fails = 0;

static void fail(const char *label, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", label, what);
    g_fails++;
}

static sigil_save_unit *resolve(const char *label, const char *layout, const char *platform,
                                const char *content, uint32_t features,
                                const sigil_save_option *opts, size_t opt_count,
                                const char *const *listing, size_t listing_count,
                                fake_root *root) {
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = layout;
    req.platform = platform;
    req.content_name = content;
    req.features = features;
    req.options = opts;
    req.option_count = opt_count;
    req.listing = listing;
    req.listing_count = listing_count;
    req.open = root ? fake_open : NULL;
    req.open_ctx = root;
    sigil_save_unit *unit = NULL;
    int rc = sigil_save_resolve(&req, &unit);
    if (rc != SIGIL_OK || !unit) {
        char msg[64];
        snprintf(msg, sizeof(msg), "resolve rc=%d", rc);
        fail(label, msg);
        return NULL;
    }
    return unit;
}

static void expect_members(const char *label, const sigil_save_unit *u, int shape,
                           const char *const *paths, size_t count) {
    if (u->shape != shape) {
        char msg[64];
        snprintf(msg, sizeof(msg), "shape=%d want %d", u->shape, shape);
        fail(label, msg);
    }
    if (u->member_count != count) {
        char msg[96];
        snprintf(msg, sizeof(msg), "member_count=%zu want %zu", u->member_count, count);
        fail(label, msg);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(u->members[i].path, paths[i]) != 0) {
            char msg[SIGIL_SAVE_PATH_MAX * 2];
            snprintf(msg, sizeof(msg), "member[%zu]='%s' want '%s'", i, u->members[i].path, paths[i]);
            fail(label, msg);
        }
    }
}

static void expect_str(const char *label, const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        char msg[SIGIL_SAVE_PATH_MAX * 2];
        snprintf(msg, sizeof(msg), "%s='%s' want '%s'", what, got, want);
        fail(label, msg);
    }
}

static void test_stem(void) {
    char out[256];
    expect_str("stem plain", sigil_content_stem("Crystal.gbc", out, sizeof(out)), "Crystal", "stem");
    expect_str("stem dir", sigil_content_stem("/roms/gbc/Pokemon - Crystal Version (USA).gbc", out, sizeof(out)),
               "Pokemon - Crystal Version (USA)", "stem");
    expect_str("stem archive member", sigil_content_stem("/roms/comp.zip#Game (USA).gb", out, sizeof(out)),
               "Game (USA)", "stem");
    expect_str("stem nested member", sigil_content_stem("comp.7z#folder/game.sfc", out, sizeof(out)), "game", "stem");
    expect_str("stem m3u", sigil_content_stem("Final Fantasy VII (USA).m3u", out, sizeof(out)),
               "Final Fantasy VII (USA)", "stem");
    expect_str("stem romset zip", sigil_content_stem("/roms/arcade/mslug.zip", out, sizeof(out)), "mslug", "stem");
    expect_str("stem dotfile", sigil_content_stem(".hidden", out, sizeof(out)), ".hidden", "stem");
    expect_str("stem multi-dot", sigil_content_stem("Game.v1.2.sfc", out, sizeof(out)), "Game.v1.2", "stem");
}

static void test_default_layout(void) {
    const char *listing[] = { "Crystal.srm", "Crystal.rtc", "Crystal 2.srm", "Crystal.state1", "Tetris.srm" };
    sigil_save_unit *u = resolve("gb rtc unit", "mgba", "gbc", "Crystal.gbc", SIGIL_FEATURE_RTC,
                                 NULL, 0, listing, 5, NULL);
    if (!u) return;
    const char *want[] = { "Crystal.srm", "Crystal.rtc" };
    expect_members("gb rtc unit", u, SIGIL_SAVE_SHAPE_MULTI, want, 2);
    expect_str("gb rtc unit", u->artifact, "Crystal.srm.zip", "artifact");
    expect_str("gb rtc unit", u->key, "Crystal", "key");
    if (u->members[1].role != SIGIL_SAVE_ROLE_RTC) fail("gb rtc unit", "rtc member role");
    if (u->expected_count != 0) fail("gb rtc unit", "nothing should be expected");
    sigil_save_unit_free(u);

    const char *listing2[] = { "Crystal.srm" };
    u = resolve("gb rtc missing", "gambatte", "gbc", "Crystal.gbc", SIGIL_FEATURE_RTC, NULL, 0, listing2, 1, NULL);
    if (!u) return;
    const char *want2[] = { "Crystal.srm" };
    expect_members("gb rtc missing", u, SIGIL_SAVE_SHAPE_SINGLE, want2, 1);
    expect_str("gb rtc missing", u->artifact, "Crystal.srm", "artifact");
    if (u->expected_count != 1 || strcmp(u->expected[0].path, "Crystal.rtc") != 0) {
        fail("gb rtc missing", "Crystal.rtc should be expected");
    }
    sigil_save_unit_free(u);

    u = resolve("gb no rtc cart", "mgba", "gb", "Tetris.gb", 0, NULL, 0, listing2, 1, NULL);
    if (!u) return;
    if (u->member_count != 0 || u->shape != SIGIL_SAVE_SHAPE_NONE) {
        fail("gb no rtc cart", "Tetris has no members in this listing");
    }
    if (u->expected_count != 1 || strcmp(u->expected[0].path, "Tetris.srm") != 0
        || u->expected[0].role != SIGIL_SAVE_ROLE_PRIMARY) {
        fail("gb no rtc cart", "only the primary Tetris.srm should be expected");
    }
    sigil_save_unit_free(u);

    u = resolve("gpsp ignores rtc flag", "gpsp", "gba", "Emerald.gba", SIGIL_FEATURE_RTC, NULL, 0, listing2, 1, NULL);
    if (!u) return;
    for (size_t i = 0; i < u->expected_count; i++) {
        if (u->expected[i].role == SIGIL_SAVE_ROLE_RTC) fail("gpsp ignores rtc flag", "no rtc region, none expected");
    }
    sigil_save_unit_free(u);

    u = resolve("unknown core takes default", "some_new_core", "snes", "Game.sfc", SIGIL_FEATURE_RTC, NULL, 0, listing2, 1, NULL);
    if (!u) return;
    if (u->expected_count != 2 || u->expected[1].role != SIGIL_SAVE_ROLE_RTC) {
        fail("unknown core takes default", "default row expects Game.srm and Game.rtc");
    }
    sigil_save_unit_free(u);

    const char *empty_listing[] = { NULL };
    u = resolve("dosbox expected primary", "dosbox_pure", "dos", "Doom.zip", 0, NULL, 0, empty_listing, 0, NULL);
    if (!u) return;
    if (u->expected_count != 1 || strcmp(u->expected[0].path, "Doom.pure.zip") != 0) {
        fail("dosbox expected primary", "an empty root still names Doom.pure.zip");
    }
    sigil_save_unit_free(u);
}

static void test_segacd(void) {
    const char *listing[] = {
        "Sonic CD (USA).srm", "Sonic CD (USA).brm", "Sonic CD (USA)_4Mbit_cart.brm",
        "scd_U.brm", "4Mbit_cart.brm", "Other.brm"
    };
    sigil_save_option per_game[] = {
        { "genesis_plus_gx_system_bram", "per game" },
        { "genesis_plus_gx_cart_bram", "per game" },
        { "genesis_plus_gx_cart_size", "4meg" },
    };
    sigil_save_unit *u = resolve("segacd per game", "genesis_plus_gx", "segacd", "Sonic CD (USA).chd", 0,
                                 per_game, 3, listing, 6, NULL);
    if (!u) return;
    const char *want[] = { "Sonic CD (USA).srm", "Sonic CD (USA).brm", "Sonic CD (USA)_4Mbit_cart.brm" };
    expect_members("segacd per game", u, SIGIL_SAVE_SHAPE_MULTI, want, 3);
    if (u->unkeyed_count != 0) fail("segacd per game", "shared files are not written under per game");
    sigil_save_unit_free(u);

    u = resolve("segacd core defaults", "genesis_plus_gx", "segacd", "Sonic CD (USA).chd", 0,
                NULL, 0, listing, 6, NULL);
    if (!u) return;
    const char *want2[] = { "Sonic CD (USA).srm" };
    expect_members("segacd core defaults", u, SIGIL_SAVE_SHAPE_SINGLE, want2, 1);
    if (u->unkeyed_count != 2 || strcmp(u->unkeyed[0], "scd_U.brm") != 0
        || strcmp(u->unkeyed[1], "4Mbit_cart.brm") != 0) {
        fail("segacd core defaults", "scd_U.brm and 4Mbit_cart.brm should be reported unkeyed");
    }
    sigil_save_unit_free(u);

    u = resolve("segacd argosy slug", "genesis_plus_gx", "scd", "Sonic CD (USA).chd", 0,
                per_game, 3, listing, 6, NULL);
    if (!u) return;
    expect_members("segacd argosy slug", u, SIGIL_SAVE_SHAPE_MULTI, want, 3);
    sigil_save_unit_free(u);

    const char *genesis_listing[] = { "Sonic (USA).srm", "Sonic (USA).brm" };
    u = resolve("genesis cart takes default", "genesis_plus_gx", "genesis", "Sonic (USA).md", 0,
                per_game, 3, genesis_listing, 2, NULL);
    if (!u) return;
    const char *want3[] = { "Sonic (USA).srm" };
    expect_members("genesis cart takes default", u, SIGIL_SAVE_SHAPE_SINGLE, want3, 1);
    sigil_save_unit_free(u);
}

static void test_psx_saturn(void) {
    const char *listing[] = {
        "Final Fantasy VII (USA).srm", "Final Fantasy VII (USA).0.mcr", "Final Fantasy VII (USA).1.mcr",
        "mednafen_psx_libretro_shared.0.mcr"
    };
    sigil_save_option card1[] = { { "beetle_psx_hw_enable_memcard1", "enabled" } };
    sigil_save_unit *u = resolve("beetle psx card1", "mednafen_psx_hw", "psx", "Final Fantasy VII (USA).m3u", 0,
                                 card1, 1, listing, 4, NULL);
    if (!u) return;
    const char *want[] = { "Final Fantasy VII (USA).srm", "Final Fantasy VII (USA).1.mcr" };
    expect_members("beetle psx card1", u, SIGIL_SAVE_SHAPE_MULTI, want, 2);
    sigil_save_unit_free(u);

    sigil_save_option mednafen[] = {
        { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" },
        { "beetle_psx_hw_shared_memory_cards", "enabled" },
    };
    u = resolve("beetle psx mednafen shared", "mednafen_psx_hw", "psx", "Final Fantasy VII (USA).m3u", 0,
                mednafen, 2, listing, 4, NULL);
    if (!u) return;
    const char *want2[] = { "Final Fantasy VII (USA).0.mcr" };
    expect_members("beetle psx mednafen shared", u, SIGIL_SAVE_SHAPE_SINGLE, want2, 1);
    if (u->unkeyed_count != 1 || strcmp(u->unkeyed[0], "mednafen_psx_libretro_shared.0.mcr") != 0) {
        fail("beetle psx mednafen shared", "shared card should be reported unkeyed");
    }
    sigil_save_unit_free(u);

    const char *saturn_listing[] = { "Panzer Dragoon Saga (USA).srm", "Panzer Dragoon Saga (USA).bcr",
                                     "Panzer Dragoon Saga (USA).smpc" };
    u = resolve("saturn cart", "mednafen_saturn", "saturn", "Panzer Dragoon Saga (USA).m3u", 0,
                NULL, 0, saturn_listing, 3, NULL);
    if (!u) return;
    const char *want3[] = { "Panzer Dragoon Saga (USA).srm", "Panzer Dragoon Saga (USA).bcr",
                            "Panzer Dragoon Saga (USA).smpc" };
    expect_members("saturn cart", u, SIGIL_SAVE_SHAPE_MULTI, want3, 3);
    sigil_save_unit_free(u);
}

static void test_single_file_cores(void) {
    struct { const char *layout; const char *content; const char *file; } cases[] = {
        { "mednafen_ngp", "Cardfight.ngc", "Cardfight.flash" },
        { "pokemini", "Zany Cards.min", "Zany Cards.eep" },
        { "handy", "Chip's Challenge.lnx", "Chip's Challenge.eeprom" },
        { "melonds", "Game.nds", "Game.sav" },
        { "dosbox_pure", "Doom.zip", "Doom.pure.zip" },
        { "opera", "Gex.chd", "opera/per_game/Gex.0.srm" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *listing[] = { cases[i].file, "unrelated.srm" };
        sigil_save_unit *u = resolve(cases[i].layout, cases[i].layout, NULL, cases[i].content, 0,
                                     NULL, 0, listing, 2, NULL);
        if (!u) continue;
        const char *want[] = { cases[i].file };
        expect_members(cases[i].layout, u, SIGIL_SAVE_SHAPE_SINGLE, want, 1);
        sigil_save_unit_free(u);
    }

    const char *subdirs[8];
    size_t n = sigil_save_layout_subdirs("mame2003_plus", subdirs, 8);
    if (n != 4 || strcmp(subdirs[0], "mame2003-plus/nvram") != 0) fail("mame subdirs", "expected four subfolders");
    if (sigil_save_layout_subdirs("mgba", subdirs, 8) != 0) fail("mgba subdirs", "default layout has none");
}

static void test_arcade(void) {
    const char *listing[] = { "fbneo/mslug.fs", "fbneo/mslug.nv", "fbneo/mslug.memcard", "fbneo/shared.memcard",
                              "fbneo/mslug2.fs" };
    sigil_save_unit *u = resolve("fbneo default", "fbneo", "arcade", "/roms/arcade/mslug.zip", 0,
                                 NULL, 0, listing, 5, NULL);
    if (!u) return;
    const char *want[] = { "fbneo/mslug.fs", "fbneo/mslug.nv" };
    expect_members("fbneo default", u, SIGIL_SAVE_SHAPE_MULTI, want, 2);
    expect_str("fbneo default", u->members[0].entry, "mslug.fs", "entry");
    expect_str("fbneo default", u->artifact, "mslug.fs.zip", "artifact");
    sigil_save_unit_free(u);

    sigil_save_option per_game[] = { { "fbneo-memcard-mode", "per-game" } };
    u = resolve("fbneo memcard", "fbneo", "arcade", "mslug.zip", 0, per_game, 1, listing, 5, NULL);
    if (!u) return;
    const char *want2[] = { "fbneo/mslug.fs", "fbneo/mslug.nv", "fbneo/mslug.memcard" };
    expect_members("fbneo memcard", u, SIGIL_SAVE_SHAPE_MULTI, want2, 3);
    sigil_save_unit_free(u);

    const char *mame_listing[] = { "mame2003-plus/nvram/mslug.nv", "mame2003-plus/hi/mslug.hi", "mame2003-plus/cfg/mslug.cfg" };
    u = resolve("mame2003 default", "mame2003_plus", "arcade", "mslug.zip", 0, NULL, 0, mame_listing, 3, NULL);
    if (!u) return;
    const char *want3[] = { "mame2003-plus/nvram/mslug.nv", "mame2003-plus/hi/mslug.hi" };
    expect_members("mame2003 default", u, SIGIL_SAVE_SHAPE_MULTI, want3, 2);
    sigil_save_unit_free(u);

    sigil_save_option flat[] = { { "mame2003-plus_core_save_subfolder", "disabled" } };
    const char *flat_listing[] = { "nvram/mslug.nv", "hi/mslug.hi" };
    u = resolve("mame2003 flat", "mame2003_plus", "arcade", "mslug.zip", 0, flat, 1, flat_listing, 2, NULL);
    if (!u) return;
    const char *want4[] = { "nvram/mslug.nv", "hi/mslug.hi" };
    expect_members("mame2003 flat", u, SIGIL_SAVE_SHAPE_MULTI, want4, 2);
    sigil_save_unit_free(u);

    const char *cdi_listing[] = { "same_cdi/nvram/Hotel Mario/cdi_nvram", "same_cdi/nvram/Hotel Mario/x.bin",
                                  "same_cdi/nvram/Other/cdi_nvram" };
    u = resolve("same_cdi folder", "same_cdi", "cdi", "Hotel Mario.chd", 0, NULL, 0, cdi_listing, 3, NULL);
    if (!u) return;
    const char *want5[] = { "same_cdi/nvram/Hotel Mario/cdi_nvram", "same_cdi/nvram/Hotel Mario/x.bin" };
    expect_members("same_cdi folder", u, SIGIL_SAVE_SHAPE_FOLDER, want5, 2);
    expect_str("same_cdi folder", u->members[0].entry, "Hotel Mario/cdi_nvram", "entry");
    sigil_save_unit_free(u);
}

static void test_hashes(void) {
    static const uint8_t hello[] = "hello";
    static const uint8_t aaaa[] = "AAAA";
    static const uint8_t bbbb[] = "BBBB";
    static const uint8_t fox[] = "The quick brown fox jumps over the lazy dog";
    static uint8_t thousand_a[1000];
    static uint8_t bytes_300[256 * 300];
    memset(thousand_a, 'a', sizeof(thousand_a));
    for (size_t i = 0; i < sizeof(bytes_300); i++) bytes_300[i] = (uint8_t)(i & 0xFF);

    fake_file files[] = {
        { "Hello.srm", hello, 5 },
        { "Crystal.srm", aaaa, 4 },
        { "Crystal.rtc", bbbb, 4 },
        { "Fox.srm", fox, sizeof(fox) - 1 },
        { "Long.srm", thousand_a, sizeof(thousand_a) },
        { "Blocks.srm", bytes_300, sizeof(bytes_300) },
        { "Empty.srm", hello, 0 },
        { "Doom.pure.zip", STORED_ZIP, sizeof(STORED_ZIP) },
        { "Quake.pure.zip", DEFLATED_ZIP, sizeof(DEFLATED_ZIP) },
        { "same_cdi/nvram/ULUS10064DATA00/PARAM.SFO", (const uint8_t *)"sfo", 3 },
        { "same_cdi/nvram/ULUS10064DATA00/DATA.BIN", (const uint8_t *)"data", 4 },
    };
    fake_root root = { files, sizeof(files) / sizeof(files[0]) };

    struct { const char *label; const char *content; const char *file; const char *hash; } raw[] = {
        { "raw hello", "Hello.gb", "Hello.srm", RAW_HELLO_HASH },
        { "raw fox", "Fox.gb", "Fox.srm", "9e107d9d372bb6826bd81d3542a419d6" },
        { "raw 1000 a", "Long.gb", "Long.srm", "cabe45dcc9ae5b66ba86600cca6b8ba8" },
        { "raw 76800 bytes", "Blocks.gb", "Blocks.srm", "97ab7d414a0df5138afc887ee74a0aff" },
        { "raw empty", "Empty.gb", "Empty.srm", "d41d8cd98f00b204e9800998ecf8427e" },
    };
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        const char *listing[] = { raw[i].file };
        sigil_save_unit *u = resolve(raw[i].label, "mgba", "gb", raw[i].content, 0, NULL, 0, listing, 1, &root);
        if (!u) continue;
        expect_str(raw[i].label, u->content_hash, raw[i].hash, "hash");
        sigil_save_unit_free(u);
    }

    const char *multi[] = { "Crystal.rtc", "Crystal.srm" };
    sigil_save_unit *u = resolve("multi hash", "mgba", "gbc", "Crystal.gbc", SIGIL_FEATURE_RTC, NULL, 0, multi, 2, &root);
    if (u) {
        expect_str("multi hash", u->content_hash, MULTI_CRYSTAL_HASH, "hash");
        sigil_save_unit_free(u);
    }

    const char *stored[] = { "Doom.pure.zip" };
    u = resolve("stored zip hash", "dosbox_pure", "dos", "Doom.zip", 0, NULL, 0, stored, 1, &root);
    if (u) {
        expect_str("stored zip hash", u->content_hash, STORED_ZIP_HASH, "hash");
        expect_str("stored zip hash", u->artifact, "Doom.pure.zip", "artifact");
        sigil_save_unit_free(u);
    }

    const char *deflated[] = { "Quake.pure.zip" };
    u = resolve("deflated zip hash", "dosbox_pure", "dos", "Quake.zip", 0, NULL, 0, deflated, 1, &root);
    if (u) {
        expect_str("deflated zip hash", u->content_hash, DEFLATED_ZIP_HASH, "hash");
        sigil_save_unit_free(u);
    }

    const char *folder[] = { "same_cdi/nvram/ULUS10064DATA00/DATA.BIN", "same_cdi/nvram/ULUS10064DATA00/PARAM.SFO" };
    u = resolve("folder hash", "same_cdi", "cdi", "ULUS10064DATA00.chd", 0, NULL, 0, folder, 2, &root);
    if (u) {
        expect_str("folder hash", u->members[0].entry, "ULUS10064DATA00/DATA.BIN", "entry");
        expect_str("folder hash", u->content_hash, FOLDER_PSP_HASH, "hash");
        expect_str("folder hash", u->artifact, "ULUS10064DATA00.zip", "artifact");
        sigil_save_unit_free(u);
    }
}

int main(void) {
    test_stem();
    test_default_layout();
    test_segacd();
    test_psx_saturn();
    test_single_file_cores();
    test_arcade();
    test_hashes();

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_save_unit: ok\n");
    return 0;
}

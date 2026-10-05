// SPDX-License-Identifier: MPL-2.0
#include "sigil.h"
#include "save_layout.h"
#include "save_unit_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    req.content_path = content;
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
    const char *want[] = { "Sonic CD (USA).brm", "Sonic CD (USA)_4Mbit_cart.brm" };
    expect_members("segacd per game", u, SIGIL_SAVE_SHAPE_MULTI, want, 2);
    if (u->unkeyed_count != 0) fail("segacd per game", "shared files are not written under per game");
    sigil_save_unit_free(u);

    u = resolve("segacd core defaults", "genesis_plus_gx", "segacd", "Sonic CD (USA).chd", 0,
                NULL, 0, listing, 6, NULL);
    if (!u) return;
    expect_members("segacd core defaults", u, SIGIL_SAVE_SHAPE_NONE, NULL, 0);
    if (u->unkeyed_count != 2 || strcmp(u->unkeyed[0], "scd_U.brm") != 0
        || strcmp(u->unkeyed[1], "4Mbit_cart.brm") != 0) {
        fail("segacd core defaults", "scd_U.brm and 4Mbit_cart.brm should be reported unkeyed");
    }
    sigil_save_unit_free(u);

    u = resolve("segacd argosy slug", "genesis_plus_gx", "scd", "Sonic CD (USA).chd", 0,
                per_game, 3, listing, 6, NULL);
    if (!u) return;
    expect_members("segacd argosy slug", u, SIGIL_SAVE_SHAPE_MULTI, want, 2);
    sigil_save_unit_free(u);

    const char *genesis_listing[] = { "Sonic (USA).srm", "Sonic (USA).brm" };
    u = resolve("genesis cart takes default", "genesis_plus_gx", "genesis", "Sonic (USA).md", 0,
                per_game, 3, genesis_listing, 2, NULL);
    if (!u) return;
    const char *want3[] = { "Sonic (USA).srm" };
    expect_members("genesis cart takes default", u, SIGIL_SAVE_SHAPE_SINGLE, want3, 1);
    sigil_save_unit_free(u);
}

/* The genesis_plus_gx internal volume a request targets: the forced region
 * option first, then the content name's first region tag, then the one
 * region file present; nothing when none of those decides. */
static void test_segacd_regions(void) {
    static const char *const E[] = { "scd_E.brm" };
    static const char *const EU[] = { "scd_E.brm", "scd_U.brm" };
    const struct {
        const char *what; const char *content; const char *region; const char *const *files; size_t file_count;
        int rc; const char *path;
    } CASES[] = {
        { "pal forced", "Lunar.cue", "pal", NULL, 0, SIGIL_OK, "scd_E.brm" },
        { "ntsc-u forced", "Lunar (Japan).cue", "ntsc-u", NULL, 0, SIGIL_OK, "scd_U.brm" },
        { "ntsc-j forced", "Lunar (USA).cue", "ntsc-j", NULL, 0, SIGIL_OK, "scd_J.brm" },
        { "Europe tag", "Lunar (Europe).cue", NULL, NULL, 0, SIGIL_OK, "scd_E.brm" },
        { "first of two tags", "Lunar (USA, Europe).cue", NULL, NULL, 0, SIGIL_OK, "scd_U.brm" },
        { "Japan tag", "Lunar (Japan) (Rev 1).cue", NULL, NULL, 0, SIGIL_OK, "scd_J.brm" },
        { "the one file present", "Lunar.cue", NULL, E, 1, SIGIL_OK, "scd_E.brm" },
        { "the tag over the file present", "Lunar (Japan).cue", NULL, E, 1, SIGIL_OK, "scd_J.brm" },
        { "no tag and no file", "Lunar.cue", NULL, NULL, 0, SIGIL_ERR_NOT_FOUND, NULL },
        { "no tag and two files", "Lunar.cue", NULL, EU, 2, SIGIL_ERR_NOT_FOUND, NULL },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        sigil_save_option region = { "genesis_plus_gx_region_detect", CASES[i].region };
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = "genesis_plus_gx";
        req.platform = "segacd";
        req.content_path = CASES[i].content;
        req.options = CASES[i].region ? &region : NULL;
        req.option_count = CASES[i].region ? 1 : 0;
        req.listing = CASES[i].files;
        req.listing_count = CASES[i].file_count;
        sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
        size_t count = 0;
        int rc = sigil_save_volume_targets(&req, targets, &count);
        const char *got = NULL;
        for (size_t t = 0; rc == SIGIL_OK && t < count; t++) {
            if (targets[t].device == SIGIL_DEVICE_INTERNAL) got = targets[t].path;
        }
        if (rc != CASES[i].rc || (CASES[i].path && (!got || strcmp(got, CASES[i].path) != 0))) fail("segacd region", CASES[i].what);
    }
}

/* Each genesis_plus_gx_cart_size value names its cart file and the size a
 * new one is created at. */
static void test_segacd_cart_sizes(void) {
    static const struct { const char *value; const char *path; uint32_t size; } CARTS[] = {
        { "128k", "128Kbit_cart.brm", 16u * 1024u }, { "256k", "256Kbit_cart.brm", 32u * 1024u },
        { "512k", "512Kbit_cart.brm", 64u * 1024u }, { "1meg", "1Mbit_cart.brm", 128u * 1024u },
        { "2meg", "2Mbit_cart.brm", 256u * 1024u }, { "4meg", "4Mbit_cart.brm", 512u * 1024u },
        { NULL, "4Mbit_cart.brm", 512u * 1024u },
    };
    for (size_t i = 0; i < sizeof(CARTS) / sizeof(CARTS[0]); i++) {
        sigil_save_option size = { "genesis_plus_gx_cart_size", CARTS[i].value };
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = "genesis_plus_gx";
        req.platform = "segacd";
        req.content_path = "Lunar (USA).cue";
        req.options = CARTS[i].value ? &size : NULL;
        req.option_count = CARTS[i].value ? 1 : 0;
        sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
        size_t count = 0;
        const sigil_volume_target *cart = NULL;
        if (sigil_save_volume_targets(&req, targets, &count) == SIGIL_OK) {
            for (size_t t = 0; t < count; t++) {
                if (targets[t].device == SIGIL_DEVICE_CART) cart = &targets[t];
            }
        }
        if (!cart || strcmp(cart->path, CARTS[i].path) != 0 || cart->new_size != CARTS[i].size) {
            fail("segacd cart size", CARTS[i].value ? CARTS[i].value : "default");
        }
    }
}

/* Dolphin's region folder comes from the disc's region letter (the serial's
 * fourth character, else the title id's last byte): E is USA, J and K are
 * JAP, any other is EUR. SlotA picks the GCI folder (8, the default) or the
 * raw card (1), and a raw card already there under a block-count name wins. */
static void test_gamecube_targets(void) {
    static const char *const SMALL[] = { "User/GC/MemoryCardA.USA.59.raw" };
    const struct {
        const char *what; const char *layout; const char *serial; const char *title_id; const char *slot;
        const char *const *files; size_t file_count; int device; const char *path;
    } CASES[] = {
        { "E", "dolphin", "GFZE", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/USA/Card A/" },
        { "J", "dolphin", "GIGJ", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/JAP/Card A/" },
        { "K", "dolphin", "GXXK", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/JAP/Card A/" },
        { "P", "dolphin", "GFZP", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/EUR/Card A/" },
        { "D", "dolphin", "GFZD", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/EUR/Card A/" },
        { "title id E", "dolphin", "", "47465A45", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/USA/Card A/" },
        { "title id J", "dolphin", "", "4749474A", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/JAP/Card A/" },
        { "serial over title id", "dolphin", "GFZP", "47465A45", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER,
          "User/GC/EUR/Card A/" },
        { "SlotA 8", "dolphin", "GFZE", "", "8", NULL, 0, SIGIL_DEVICE_GC_FOLDER, "User/GC/USA/Card A/" },
        { "SlotA 1", "dolphin", "GFZE", "", "1", NULL, 0, SIGIL_DEVICE_GC_CARD, "User/GC/MemoryCardA.USA.raw" },
        { "SlotA 1 small card", "dolphin", "GFZE", "", "1", SMALL, 1, SIGIL_DEVICE_GC_CARD,
          "User/GC/MemoryCardA.USA.59.raw" },
        { "standalone", "dolphin_standalone", "GFZE", "", NULL, NULL, 0, SIGIL_DEVICE_GC_FOLDER, "GC/USA/Card A/" },
        { "standalone SlotA 1", "dolphin_standalone", "GIGJ", "", "1", NULL, 0, SIGIL_DEVICE_GC_CARD,
          "GC/MemoryCardA.JAP.raw" },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        sigil_result result;
        memset(&result, 0, sizeof(result));
        result.struct_version = SIGIL_RESULT_V3;
        result.platform = SIGIL_PLATFORM_GAMECUBE;
        snprintf(result.raw_serial, sizeof(result.raw_serial), "%s", CASES[i].serial);
        snprintf(result.title_id, sizeof(result.title_id), "%s", CASES[i].title_id);
        sigil_save_option slot = { "SlotA", CASES[i].slot };
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = CASES[i].layout;
        req.platform = "gamecube";
        req.content_path = "Game.iso";
        req.result = &result;
        req.options = CASES[i].slot ? &slot : NULL;
        req.option_count = CASES[i].slot ? 1 : 0;
        req.listing = CASES[i].files;
        req.listing_count = CASES[i].file_count;
        sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
        size_t count = 0;
        int rc = sigil_save_volume_targets(&req, targets, &count);
        if (rc != SIGIL_OK || count != 1 || targets[0].device != CASES[i].device ||
            strcmp(targets[0].path, CASES[i].path) != 0) {
            fail("gamecube target", CASES[i].what);
        }
    }

    sigil_result none;
    memset(&none, 0, sizeof(none));
    none.struct_version = SIGIL_RESULT_V3;
    none.platform = SIGIL_PLATFORM_GAMECUBE;
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = "dolphin";
    req.platform = "gamecube";
    req.content_path = "Game.iso";
    req.result = &none;
    sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
    size_t count = 0;
    if (sigil_save_volume_targets(&req, targets, &count) == SIGIL_OK && count != 0) {
        fail("gamecube target", "a game with no region letter got a folder");
    }
}

/* Every flycast file names its VMU port, and the target for each port's
 * device is that port's file, under every setting. */
static void test_vmu_ports(void) {
    static const char *const PORTS[] = { "A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2" };
    static const struct { const char *layout; const char *key; const char *value; size_t count; } SETTINGS[] = {
        { "flycast", "reicast_per_content_vmus", "disabled", 8 },
        { "flycast", "reicast_per_content_vmus", "All VMUs", 8 },
        { "flycast", "reicast_per_content_vmus", "VMU A1", 1 },
        { "flycast_standalone", "PerGameVmu", "yes", 8 },
        { "flycast_standalone", "PerGameVmu", "no", 8 },
    };
    for (size_t s = 0; s < sizeof(SETTINGS) / sizeof(SETTINGS[0]); s++) {
        sigil_result result;
        memset(&result, 0, sizeof(result));
        result.struct_version = SIGIL_RESULT_V3;
        snprintf(result.title_id, sizeof(result.title_id), "T1");
        sigil_save_option option = { SETTINGS[s].key, SETTINGS[s].value };
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = SETTINGS[s].layout;
        req.platform = "dc";
        req.content_path = "G.gdi";
        req.result = &result;
        req.options = &option;
        req.option_count = 1;
        sigil_volume_target targets[SIGIL_VOLUME_TARGETS_MAX];
        size_t count = 0;
        if (sigil_save_volume_targets(&req, targets, &count) != SIGIL_OK || count != SETTINGS[s].count) {
            fail("vmu ports", SETTINGS[s].value);
            continue;
        }
        for (size_t t = 0; t < count; t++) {
            int port = targets[t].device - SIGIL_DEVICE_VMU_A1;
            const char *file = strrchr(targets[t].path, '/');
            file = file ? file + 1 : targets[t].path;
            if (port < 0 || port >= 8 || !strstr(file, PORTS[port])) fail("vmu ports", targets[t].path);
        }
    }
}

#define X3_MAX 10

typedef struct {
    const char *what;
    const char *layout;
    const char *platform;
    const char *content;
    sigil_save_option options[3];
    const char *const *listing;
    size_t listing_count;
    const char *members[X3_MAX];
    const char *unkeyed[X3_MAX];
    const char *title_id;   /* NULL for "T1" */
    const char *raw_serial; /* NULL for empty */
    const char *save_id;    /* NULL for empty */
} option_case;

static size_t count_of(const char *const *list) {
    size_t n = 0;
    while (n < X3_MAX && list[n]) n++;
    return n;
}

/* Each layout row's option values, with a root holding every file the row
 * can name: the members and shared files sigil picks are exactly the ones the
 * core reads under those values. */
static void test_option_values(void) {
    static const char *const PSX[] = {
        "G.srm", "G.0.mcr", "G.1.mcr", "G.3.mcr", "mednafen_psx_libretro_shared.0.mcr",
        "mednafen_psx_libretro_shared.1.mcr", "mednafen_psx_libretro_shared.3.mcr", "pcsx-card2.mcd",
    };
    static const char *const PCSX[] = {
        "G.srm", "SLUS-00594_1.mcd", "SLUS-00594_2.mcd", "slus-00594_1.mcd", "SLUS-01206_1.mcd", "SLUSP-0120_1.mcd",
        "pcsx-card1.mcd", "pcsx-card2.mcd",
    };
    static const char *const PS2[] = { "G.ps2", "Mcd001.ps2", "Mcd002.ps2" };
    static const char *const SATURN[] = {
        "G.srm", "G.bkr", "G.bcr", "G.smpc", "mednafen_saturn_libretro_shared.bkr",
        "mednafen_saturn_libretro_shared.smpc", "mednafen_saturn_libretro_shared.bcr",
    };
    static const char *const KRONOS[] = {
        "kronos/saturn/G.ram", "kronos/saturn/G-ext512K.ram", "kronos/saturn/G-ext1M.ram",
        "kronos/saturn/G-ext2M.ram", "kronos/saturn/G-ext4M.ram", "G.bkr", "G.bcr",
    };
    static const char *const FLYCAST[] = {
        "T1.A1.bin", "T1.B1.bin", "T1.D2.bin", "vmu_save_A1.bin", "vmu_save_B1.bin",
    };
    static const char *const FLYCAST_SA[] = { "T1_vmu_save_A1.bin", "vmu_save_A1.bin", "vmu_save_B1.bin" };
    static const char *const OPERA[] = {
        "opera/per_game/G.0.srm", "opera/per_game/G.1.srm", "opera/shared/nvram.0.srm", "opera/shared/nvram.1.srm",
    };
    static const char *const FBNEO[] = { "fbneo/mslug.fs", "fbneo/mslug.nv", "fbneo/mslug.memcard", "fbneo/shared.memcard" };
    static const char *const MAME[] = {
        "mame2003-plus/nvram/mslug.nv", "mame2003-plus/hi/mslug.hi", "nvram/mslug.nv", "hi/mslug.hi",
    };
    static const char *const FDS[] = { "G.srm", "G.sav", "G.ups", "G.ips" };
    static const char *const N64[] = { "G.srm", "G.rtc", "G.eep" };
    static const char *const SWAN[] = {
        "G.srm", "G_1.mcd", "G_2.mcd", "SLUS-00664_1.mcd", "SLUS-00664_2.mcd", "duckstation_shared_card_1.mcd",
        "duckstation_shared_card_2.mcd",
    };
    static const char *const DUCK[] = {
        "memcards/G_1.mcd", "memcards/G_2.mcd", "memcards/SLUS-00664_1.mcd", "memcards/SLUS-00664_2.mcd",
        "memcards/shared_card_1.mcd", "memcards/shared_card_2.mcd", "memcards/Other_1.mcd",
    };
    static const char *const POPS[] = {
        "PSP/SAVEDATA/SLUS01040/SCEVMC0.VMP", "PSP/SAVEDATA/SLUS01040/SCEVMC1.VMP", "PSP/SAVEDATA/SLUS01040/PARAM.SFO",
        "PSP/SAVEDATA/SLUS-01040/SCEVMC0.VMP",
    };
#define L(a) a, sizeof(a) / sizeof(a[0])
    static const option_case CASES[] = {
        { "psx default", "mednafen_psx_hw", "psx", "G.cue", { { 0 } }, L(PSX), { "G.srm" }, { 0 } },
        { "psx mednafen", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" } }, L(PSX), { "G.0.mcr" }, { 0 } },
        { "psx mednafen both", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" }, { "beetle_psx_hw_enable_memcard1", "enabled" } },
          L(PSX), { "G.0.mcr", "G.1.mcr" }, { 0 } },
        { "psx left index", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" }, { "beetle_psx_hw_memcard_left_index", "3" } },
          L(PSX), { "G.3.mcr" }, { 0 } },
        { "psx right index", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_enable_memcard1", "enabled" }, { "beetle_psx_hw_memcard_right_index", "3" } }, L(PSX),
          { "G.srm", "G.3.mcr" }, { 0 } },
        { "psx shared", "mednafen_psx_hw", "psx", "G.cue", { { "beetle_psx_hw_shared_memory_cards", "enabled" } }, L(PSX),
          { "G.srm" }, { 0 } },
        { "psx shared card 2", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_shared_memory_cards", "enabled" }, { "beetle_psx_hw_enable_memcard1", "enabled" } }, L(PSX),
          { "G.srm" }, { "mednafen_psx_libretro_shared.1.mcr" } },
        { "psx shared card 2 index", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_shared_memory_cards", "enabled" }, { "beetle_psx_hw_enable_memcard1", "enabled" },
            { "beetle_psx_hw_memcard_right_index", "3" } },
          L(PSX), { "G.srm" }, { "mednafen_psx_libretro_shared.3.mcr" } },
        { "psx shared mednafen", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_shared_memory_cards", "enabled" }, { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" } },
          L(PSX), { 0 }, { "mednafen_psx_libretro_shared.0.mcr" } },
        { "psx shared mednafen index", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_shared_memory_cards", "enabled" }, { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" },
            { "beetle_psx_hw_memcard_left_index", "3" } },
          L(PSX), { 0 }, { "mednafen_psx_libretro_shared.3.mcr" } },
        { "psx shared mednafen both", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_shared_memory_cards", "enabled" }, { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" },
            { "beetle_psx_hw_enable_memcard1", "enabled" } },
          L(PSX), { 0 }, { "mednafen_psx_libretro_shared.0.mcr", "mednafen_psx_libretro_shared.1.mcr" } },
        { "psx software default", "mednafen_psx", "psx", "G.cue", { { 0 } }, L(PSX), { "G.srm" }, { 0 } },
        { "psx software mednafen index", "mednafen_psx", "psx", "G.cue",
          { { "beetle_psx_use_mednafen_memcard0_method", "mednafen" }, { "beetle_psx_memcard_left_index", "3" } },
          L(PSX), { "G.3.mcr" }, { 0 } },
        { "psx software card 2", "mednafen_psx", "psx", "G.cue", { { "beetle_psx_enable_memcard1", "enabled" } }, L(PSX),
          { "G.srm", "G.1.mcr" }, { 0 } },
        { "psx software shared mednafen", "mednafen_psx", "psx", "G.cue",
          { { "beetle_psx_shared_memory_cards", "enabled" }, { "beetle_psx_use_mednafen_memcard0_method", "mednafen" },
            { "beetle_psx_enable_memcard1", "enabled" } },
          L(PSX), { 0 }, { "mednafen_psx_libretro_shared.0.mcr", "mednafen_psx_libretro_shared.1.mcr" } },
        { "psx software ignores hw keys", "mednafen_psx", "psx", "G.cue",
          { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" } }, L(PSX), { "G.srm" }, { 0 } },
        { "psx software ignores hw index", "mednafen_psx", "psx", "G.cue",
          { { "beetle_psx_use_mednafen_memcard0_method", "mednafen" }, { "beetle_psx_hw_memcard_left_index", "3" } },
          L(PSX), { "G.0.mcr" }, { 0 } },
        { "psx hw ignores software index", "mednafen_psx_hw", "psx", "G.cue",
          { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" }, { "beetle_psx_memcard_left_index", "3" } },
          L(PSX), { "G.0.mcr" }, { 0 } },
        { "pcsx_rearmed default", "pcsx_rearmed", "psx", "G.cue", { { 0 } }, L(PCSX), { "G.srm" }, { "pcsx-card2.mcd" },
          "SLUS-00594", "SLUS_005.94" },
        { "pcsx_rearmed card 2 off", "pcsx_rearmed", "psx", "G.cue", { { "pcsx_rearmed_memcard2", "none" } }, L(PCSX),
          { "G.srm" }, { 0 }, "SLUS-00594", "SLUS_005.94" },
        { "pcsx_rearmed card 1 off", "pcsx_rearmed", "psx", "G.cue",
          { { "pcsx_rearmed_memcard1", "none" }, { "pcsx_rearmed_memcard2", "none" } }, L(PCSX), { 0 }, { 0 },
          "SLUS-00594", "SLUS_005.94" },
        { "pcsx_rearmed serial", "pcsx_rearmed", "psx", "G.cue",
          { { "pcsx_rearmed_memcard1", "serial" }, { "pcsx_rearmed_memcard2", "serial" } }, L(PCSX),
          { "SLUS-00594_1.mcd", "SLUS-00594_2.mcd" }, { 0 }, "SLUS-00594", "SLUS_005.94" },
        { "pcsx_rearmed shared", "pcsx_rearmed", "psx", "G.cue", { { "pcsx_rearmed_memcard1", "shared" } }, L(PCSX), { 0 },
          { "pcsx-card1.mcd", "pcsx-card2.mcd" }, "SLUS-00594", "SLUS_005.94" },
        { "pcsx_rearmed serial lowercase boot", "pcsx_rearmed", "psx", "G.cue", { { "pcsx_rearmed_memcard1", "serial" } },
          L(PCSX), { "slus-00594_1.mcd" }, { "pcsx-card2.mcd" }, "SLUS-00594", "slus_005.94" },
        { "pcsx_rearmed serial cut at 9", "pcsx_rearmed", "psx", "G.cue", { { "pcsx_rearmed_memcard1", "serial" } },
          L(PCSX), { "SLUSP-0120_1.mcd" }, { "pcsx-card2.mcd" }, "SLUS-01206", "SLUSP012.06" },
        { "pcsx_rearmed serial from title id", "pcsx_rearmed", "psx", "G.cue", { { "pcsx_rearmed_memcard1", "serial" } },
          L(PCSX), { "SLUS-01206_1.mcd" }, { "pcsx-card2.mcd" }, "SLUS-01206", NULL },
        { "lrps2 default", "pcsx2", "ps2", "G.iso", { { 0 } }, L(PS2), { 0 }, { "Mcd001.ps2", "Mcd002.ps2" } },
        { "lrps2 per game", "pcsx2", "ps2", "G.iso", { { "pcsx2_shared_memory_cards", "disabled" } }, L(PS2), { "G.ps2" },
          { 0 } },
        { "beetle saturn default", "mednafen_saturn", "saturn", "G.cue", { { 0 } }, L(SATURN), { "G.srm", "G.bcr", "G.smpc" },
          { 0 } },
        { "beetle saturn mednafen", "mednafen_saturn", "saturn", "G.cue", { { "beetle_saturn_save_method", "mednafen" } },
          L(SATURN), { "G.bkr", "G.bcr", "G.smpc" }, { 0 } },
        { "beetle saturn shared int", "mednafen_saturn", "saturn", "G.cue", { { "beetle_saturn_shared_int", "enabled" } },
          L(SATURN), { "G.srm", "G.bcr" }, { "mednafen_saturn_libretro_shared.smpc" } },
        { "beetle saturn mednafen shared int", "mednafen_saturn", "saturn", "G.cue",
          { { "beetle_saturn_save_method", "mednafen" }, { "beetle_saturn_shared_int", "enabled" } }, L(SATURN), { "G.bcr" },
          { "mednafen_saturn_libretro_shared.bkr", "mednafen_saturn_libretro_shared.smpc" } },
        { "beetle saturn shared ext", "mednafen_saturn", "saturn", "G.cue", { { "beetle_saturn_shared_ext", "enabled" } },
          L(SATURN), { "G.srm", "G.smpc" }, { "mednafen_saturn_libretro_shared.bcr" } },
        { "kronos default", "kronos", "saturn", "G.cue", { { 0 } }, L(KRONOS),
          { "kronos/saturn/G.ram", "kronos/saturn/G-ext512K.ram" }, { 0 } },
        { "kronos 1M cart", "kronos", "saturn", "G.cue", { { "kronos_addon_cartridge", "1M_backup_ram" } }, L(KRONOS),
          { "kronos/saturn/G.ram", "kronos/saturn/G-ext1M.ram" }, { 0 } },
        { "kronos 2M cart", "kronos", "saturn", "G.cue", { { "kronos_addon_cartridge", "2M_backup_ram" } }, L(KRONOS),
          { "kronos/saturn/G.ram", "kronos/saturn/G-ext2M.ram" }, { 0 } },
        { "kronos 4M cart", "kronos", "saturn", "G.cue", { { "kronos_addon_cartridge", "4M_backup_ram" } }, L(KRONOS),
          { "kronos/saturn/G.ram", "kronos/saturn/G-ext4M.ram" }, { 0 } },
        { "kronos beetle saves", "kronos", "saturn", "G.cue", { { "kronos_use_beetle_saves", "enabled" } }, L(KRONOS),
          { "G.bkr", "G.bcr" }, { 0 } },
        { "flycast default", "flycast", "dc", "G.gdi", { { 0 } }, L(FLYCAST), { 0 }, { "vmu_save_A1.bin", "vmu_save_B1.bin" } },
        { "flycast VMU A1", "flycast", "dc", "G.gdi", { { "reicast_per_content_vmus", "VMU A1" } }, L(FLYCAST),
          { "T1.A1.bin" }, { 0 } },
        { "flycast All VMUs", "flycast", "dc", "G.gdi", { { "reicast_per_content_vmus", "All VMUs" } }, L(FLYCAST),
          { "T1.A1.bin", "T1.B1.bin", "T1.D2.bin" }, { 0 } },
        { "flycast standalone default", "flycast_standalone", "dc", "G.gdi", { { 0 } }, L(FLYCAST_SA),
          { "T1_vmu_save_A1.bin" }, { "vmu_save_B1.bin" } },
        { "flycast standalone shared", "flycast_standalone", "dc", "G.gdi", { { "PerGameVmu", "no" } }, L(FLYCAST_SA), { 0 },
          { "vmu_save_A1.bin", "vmu_save_B1.bin" } },
        { "opera default", "opera", "3do", "G.iso", { { 0 } }, L(OPERA), { "opera/per_game/G.0.srm" }, { 0 } },
        { "opera nvram 1", "opera", "3do", "G.iso", { { "opera_nvram_version", "1" } }, L(OPERA),
          { "opera/per_game/G.1.srm" }, { 0 } },
        { "opera shared", "opera", "3do", "G.iso", { { "opera_nvram_storage", "shared" } }, L(OPERA), { 0 },
          { "opera/shared/nvram.0.srm" } },
        { "fbneo default", "fbneo", "arcade", "mslug.zip", { { 0 } }, L(FBNEO), { "fbneo/mslug.fs", "fbneo/mslug.nv" }, { 0 } },
        { "fbneo memcard per game", "fbneo", "arcade", "mslug.zip", { { "fbneo-memcard-mode", "per-game" } }, L(FBNEO),
          { "fbneo/mslug.fs", "fbneo/mslug.nv", "fbneo/mslug.memcard" }, { 0 } },
        { "fbneo memcard shared", "fbneo", "arcade", "mslug.zip", { { "fbneo-memcard-mode", "shared" } }, L(FBNEO),
          { "fbneo/mslug.fs", "fbneo/mslug.nv" }, { "fbneo/shared.memcard" } },
        { "mame2003+ default", "mame2003_plus", "arcade", "mslug.zip", { { 0 } }, L(MAME),
          { "mame2003-plus/nvram/mslug.nv", "mame2003-plus/hi/mslug.hi" }, { 0 } },
        { "mame2003+ no subfolder", "mame2003_plus", "arcade", "mslug.zip",
          { { "mame2003-plus_core_save_subfolder", "disabled" } }, L(MAME), { "nvram/mslug.nv", "hi/mslug.hi" }, { 0 } },
        { "fds default", "nestopia", "fds", "G.fds", { { 0 } }, L(FDS), { "G.sav" }, { 0 } },
        { "fds sav", "nestopia", "fds", "G.fds", { { "nestopia_fds_savefile_format", "sav_ups" } }, L(FDS), { "G.sav" },
          { 0 } },
        { "fds ups", "nestopia", "fds", "G.fds", { { "nestopia_fds_savefile_format", "ups" } }, L(FDS), { "G.ups" },
          { 0 } },
        { "fds ips", "nestopia", "fds", "G.fds", { { "nestopia_fds_savefile_format", "ips" } }, L(FDS), { "G.ips" },
          { 0 } },
        { "swanstation default", "swanstation", "psx", "G.cue", { { 0 } }, L(SWAN), { "G.srm" }, { 0 }, "SLUS-00664" },
        { "swanstation per game", "swanstation", "psx", "G.cue", { { "swanstation_MemoryCards_Card1Type", "PerGame" } },
          L(SWAN), { "SLUS-00664_1.mcd" }, { 0 }, "SLUS-00664" },
        { "swanstation per title", "swanstation", "psx", "G.cue",
          { { "swanstation_MemoryCards_Card1Type", "PerGameTitle" } }, L(SWAN), { "G_1.mcd" }, { 0 }, "SLUS-00664" },
        { "swanstation slot 2", "swanstation", "psx", "G.cue", { { "swanstation_MemoryCards_Card2Type", "PerGame" } },
          L(SWAN), { "G.srm", "SLUS-00664_2.mcd" }, { 0 }, "SLUS-00664" },
        { "swanstation shared", "swanstation", "psx", "G.cue",
          { { "swanstation_MemoryCards_Card1Type", "Shared" }, { "swanstation_MemoryCards_Card2Type", "Shared" } },
          L(SWAN), { 0 }, { "duckstation_shared_card_1.mcd", "duckstation_shared_card_2.mcd" }, "SLUS-00664" },
        { "duckstation default", "duckstation", "psx", "G.cue", { { 0 } }, L(DUCK), { "memcards/G_1.mcd" }, { 0 },
          "SLUS-00664" },
        { "duckstation per game", "duckstation", "psx", "G.cue", { { "Card1Type", "PerGame" }, { "Card2Type", "PerGame" } },
          L(DUCK), { "memcards/SLUS-00664_1.mcd", "memcards/SLUS-00664_2.mcd" }, { 0 }, "SLUS-00664" },
        { "duckstation file title", "duckstation", "psx", "G.cue", { { "Card1Type", "PerGameFileTitle" } }, L(DUCK),
          { "memcards/G_1.mcd" }, { 0 }, "SLUS-00664" },
        { "duckstation shared", "duckstation", "psx", "G.cue", { { "Card1Type", "Shared" } }, L(DUCK), { 0 },
          { "memcards/shared_card_1.mcd" }, "SLUS-00664" },
        { "armsx1", "armsx1", "psx", "G.cue", { { 0 } }, L(SWAN), { 0 }, { 0 }, "SLUS-00664" },
        { "libretro names the default row", "libretro", "psx", "G.cue",
          { { "swanstation_MemoryCards_Card1Type", "PerGame" } }, L(SWAN), { "G.srm" }, { 0 }, "SLUS-00664" },
        { "mupen64plus_next", "mupen64plus_next", "n64", "G.z64", { { 0 } }, L(N64), { "G.srm" }, { 0 } },
        { "parallel_n64", "parallel_n64", "n64", "G.z64", { { 0 } }, L(N64), { "G.srm" }, { 0 } },
        { "vita pops", "vita_pops", "psx", "Vagrant Story (USA).cue", { { 0 } }, L(POPS),
          { "PSP/SAVEDATA/SLUS01040/SCEVMC0.VMP", "PSP/SAVEDATA/SLUS01040/SCEVMC1.VMP" }, { 0 }, "SLUS-01040" },
        { "vita pops later disc", "vita_pops", "psx", "Vagrant Story (USA) (Disc 2).cue", { { 0 } }, L(POPS),
          { "PSP/SAVEDATA/SLUS01040/SCEVMC0.VMP", "PSP/SAVEDATA/SLUS01040/SCEVMC1.VMP" }, { 0 }, "SLUS-01041", NULL,
          "SLUS-01040" },
    };
#undef L
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const option_case *c = &CASES[i];
        size_t opts = 0;
        while (opts < 3 && c->options[opts].key) opts++;
        sigil_result result;
        memset(&result, 0, sizeof(result));
        result.struct_version = SIGIL_RESULT_V3;
        snprintf(result.title_id, sizeof(result.title_id), "%s", c->title_id ? c->title_id : "T1");
        snprintf(result.raw_serial, sizeof(result.raw_serial), "%s", c->raw_serial ? c->raw_serial : "");
        snprintf(result.save_id, sizeof(result.save_id), "%s", c->save_id ? c->save_id : "");
        sigil_save_request req;
        memset(&req, 0, sizeof(req));
        req.struct_version = SIGIL_SAVE_REQUEST_V1;
        req.layout = c->layout;
        req.platform = c->platform;
        req.content_path = c->content;
        req.result = &result;
        req.options = c->options;
        req.option_count = opts;
        req.listing = c->listing;
        req.listing_count = c->listing_count;
        sigil_save_unit *u = NULL;
        if (sigil_save_resolve(&req, &u) != SIGIL_OK || !u) { fail(c->what, "resolve failed"); continue; }
        size_t want_members = count_of(c->members), want_unkeyed = count_of(c->unkeyed);
        bool same = u->member_count == want_members && u->unkeyed_count == want_unkeyed;
        for (size_t m = 0; same && m < want_members; m++) same = strcmp(u->members[m].path, c->members[m]) == 0;
        for (size_t m = 0; same && m < want_unkeyed; m++) same = strcmp(u->unkeyed[m], c->unkeyed[m]) == 0;
        if (!same) {
            for (size_t m = 0; m < u->member_count; m++) fprintf(stderr, "  member %s\n", u->members[m].path);
            for (size_t m = 0; m < u->unkeyed_count; m++) fprintf(stderr, "  shared %s\n", u->unkeyed[m]);
            fail(c->what, "members or shared files differ from what the core reads");
        }
        sigil_save_unit_free(u);
    }
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
    if (u->member_count != 0) fail("beetle psx mednafen shared", "the game's own .0.mcr is unused with shared cards");
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

    const char *shared_listing[] = {
        "Panzer Dragoon Saga (USA).srm", "Panzer Dragoon Saga (USA).bkr", "Panzer Dragoon Saga (USA).bcr",
        "Panzer Dragoon Saga (USA).smpc", "mednafen_saturn_libretro_shared.bkr",
        "mednafen_saturn_libretro_shared.smpc", "mednafen_saturn_libretro_shared.bcr",
    };
    sigil_save_option all_shared[] = {
        { "beetle_saturn_save_method", "mednafen" },
        { "beetle_saturn_shared_int", "enabled" },
        { "beetle_saturn_shared_ext", "enabled" },
    };
    u = resolve("saturn mednafen shared", "mednafen_saturn", "saturn", "Panzer Dragoon Saga (USA).m3u", 0,
                all_shared, 3, shared_listing, 7, NULL);
    if (!u) return;
    expect_members("saturn mednafen shared", u, SIGIL_SAVE_SHAPE_NONE, NULL, 0);
    if (u->unkeyed_count != 3) fail("saturn mednafen shared", "the shared .bkr, .smpc and .bcr should be unkeyed");
    sigil_save_unit_free(u);

    sigil_save_option int_shared[] = { { "beetle_saturn_shared_int", "enabled" } };
    u = resolve("saturn libretro shared int", "mednafen_saturn", "saturn", "Panzer Dragoon Saga (USA).m3u", 0,
                int_shared, 1, shared_listing, 7, NULL);
    if (!u) return;
    const char *want4[] = { "Panzer Dragoon Saga (USA).srm", "Panzer Dragoon Saga (USA).bcr" };
    expect_members("saturn libretro shared int", u, SIGIL_SAVE_SHAPE_MULTI, want4, 2);
    if (u->unkeyed_count != 1 || strcmp(u->unkeyed[0], "mednafen_saturn_libretro_shared.smpc") != 0) {
        fail("saturn libretro shared int", "only the shared .smpc should be unkeyed");
    }
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
        expect_str("multi identity", u->identity_hash, "098890dde069e9abad63f19a0d9e1f32", "identity");
        sigil_save_unit_free(u);
    }

    const char *srm_only[] = { "Crystal.srm" };
    u = resolve("single identity", "mgba", "gbc", "Crystal.gbc", SIGIL_FEATURE_RTC, NULL, 0, srm_only, 1, &root);
    if (u) {
        expect_str("single identity", u->identity_hash, u->content_hash, "identity");
        expect_str("single identity", u->identity_hash, "098890dde069e9abad63f19a0d9e1f32", "identity");
        sigil_save_unit_free(u);
    }

    u = resolve("bsnes clock identity", "bsnes", "snes", "Crystal.sfc", 0, NULL, 0, multi, 2, &root);
    if (u) {
        expect_str("bsnes clock identity", u->identity_hash, "098890dde069e9abad63f19a0d9e1f32", "identity");
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

    struct { const char *label; const char *layout; const char *platform; const char *content;
             uint32_t features; const char *const *listing; size_t count; } later[] = {
        { "later raw", "mgba", "gb", "Hello.gb", 0, (const char *const[]){ "Hello.srm" }, 1 },
        { "later multi", "mgba", "gbc", "Crystal.gbc", SIGIL_FEATURE_RTC, multi, 2 },
        { "later zip", "dosbox_pure", "dos", "Doom.zip", 0, stored, 1 },
        { "later folder", "same_cdi", "cdi", "ULUS10064DATA00.chd", 0, folder, 2 },
    };
    for (size_t i = 0; i < sizeof(later) / sizeof(later[0]); i++) {
        sigil_save_unit *named = resolve(later[i].label, later[i].layout, later[i].platform, later[i].content,
                                         later[i].features, NULL, 0, later[i].listing, later[i].count, NULL);
        sigil_save_unit *hashed = resolve(later[i].label, later[i].layout, later[i].platform, later[i].content,
                                          later[i].features, NULL, 0, later[i].listing, later[i].count, &root);
        if (!named || !hashed) continue;
        expect_str(later[i].label, named->content_hash, "", "hash before sigil_save_hash");
        expect_str(later[i].label, named->identity_hash, "", "identity before sigil_save_hash");
        int rc = sigil_save_hash(named, fake_open, &root);
        if (rc != SIGIL_OK) fail(later[i].label, "sigil_save_hash failed");
        expect_str(later[i].label, named->content_hash, hashed->content_hash, "hash");
        expect_str(later[i].label, named->identity_hash, hashed->identity_hash, "identity");
        sigil_save_unit_free(named);
        sigil_save_unit_free(hashed);
    }

    const char *missing[] = { "Missing.srm" };
    u = resolve("hash missing member", "mgba", "gb", "Missing.gb", 0, NULL, 0, missing, 1, NULL);
    if (u) {
        if (sigil_save_hash(u, fake_open, &root) != SIGIL_ERR_IO) fail("hash missing member", "want SIGIL_ERR_IO");
        if (sigil_save_hash(u, NULL, &root) != SIGIL_ERR_INVALID_ARG) fail("hash without open", "want SIGIL_ERR_INVALID_ARG");
        sigil_save_unit_free(u);
    }
}

typedef struct {
    const char       *path;
    int               shared;
    sigil_save_option options[2];
} alternate_want;

static void expect_alternates(const char *label, const sigil_save_unit *u, const alternate_want *want, size_t count) {
    bool same = u->alternate_count == count;
    for (size_t i = 0; same && i < count; i++) {
        const sigil_save_alternate *a = &u->alternates[i];
        size_t opts = want[i].options[1].key ? 2 : want[i].options[0].key ? 1 : 0;
        same = strcmp(a->path, want[i].path) == 0 && a->shared == want[i].shared && a->option_count == opts;
        for (size_t o = 0; same && o < opts; o++) {
            same = strcmp(a->options[o].key, want[i].options[o].key) == 0 &&
                   strcmp(a->options[o].value, want[i].options[o].value) == 0;
        }
    }
    if (same) return;
    for (size_t i = 0; i < u->alternate_count; i++) {
        const sigil_save_alternate *a = &u->alternates[i];
        fprintf(stderr, "  alternate %s shared=%d", a->path, a->shared);
        for (size_t o = 0; o < a->option_count; o++) fprintf(stderr, " %s=%s", a->options[o].key, a->options[o].value);
        fprintf(stderr, "\n");
    }
    fail(label, "alternates differ");
}

/* Files the sent options leave out are reported with the values that take
 * them; files the options already take, and files no option selects, are not. */
static void test_alternates(void) {
    const char *era1[] = { "G.bkr", "G.bcr", "G.smpc" };
    sigil_save_unit *u = resolve("saturn bkr only", "mednafen_saturn", "saturn", "G.cue", 0, NULL, 0, era1, 3, NULL);
    if (u) {
        const alternate_want want[] = { { "G.bkr", 0, { { "beetle_saturn_save_method", "mednafen" } } } };
        expect_alternates("saturn bkr only", u, want, 1);
        sigil_save_unit_free(u);
    }

    const char *both[] = { "G.srm", "G.bkr" };
    sigil_save_option mednafen[] = { { "beetle_saturn_save_method", "mednafen" } };
    u = resolve("saturn mednafen", "mednafen_saturn", "saturn", "G.cue", 0, mednafen, 1, both, 2, NULL);
    if (u) {
        const alternate_want want[] = { { "G.srm", 0, { { "beetle_saturn_save_method", "libretro" } } } };
        expect_alternates("saturn mednafen", u, want, 1);
        sigil_save_unit_free(u);
    }

    const char *psx[] = { "G.srm", "G.0.mcr", "mednafen_psx_libretro_shared.0.mcr" };
    sigil_save_option shared[] = { { "beetle_psx_hw_shared_memory_cards", "enabled" } };
    u = resolve("psx two options", "mednafen_psx_hw", "psx", "G.cue", 0, shared, 1, psx, 3, NULL);
    if (u) {
        const alternate_want want[] = {
            { "G.0.mcr", 0,
              { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" },
                { "beetle_psx_hw_shared_memory_cards", "disabled" } } },
            { "mednafen_psx_libretro_shared.0.mcr", 1, { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" } } },
        };
        expect_alternates("psx two options", u, want, 2);
        sigil_save_unit_free(u);
    }

    const char *taken[] = { "G.srm", "pcsx-card2.mcd", "G.sav" };
    u = resolve("nothing left out", "pcsx_rearmed", "psx", "G.cue", 0, NULL, 0, taken, 3, NULL);
    if (u) {
        expect_alternates("nothing left out", u, NULL, 0);
        sigil_save_unit_free(u);
    }

    const char *same_file[] = { "G.0.mcr" };
    sigil_save_option slot2[] = {
        { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" },
        { "beetle_psx_hw_memcard_right_index", "0" },
    };
    u = resolve("taken by another member", "mednafen_psx_hw", "psx", "G.cue", 0, slot2, 2, same_file, 1, NULL);
    if (u) {
        expect_alternates("taken by another member", u, NULL, 0);
        sigil_save_unit_free(u);
    }

    sigil_save_option same_index[] = { { "beetle_psx_hw_memcard_right_index", "0" } };
    u = resolve("one file, two members", "mednafen_psx_hw", "psx", "G.cue", 0, same_index, 1, same_file, 1, NULL);
    if (u) {
        const alternate_want want[] = {
            { "G.0.mcr", 0, { { "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen" } } },
        };
        expect_alternates("one file, two members", u, want, 1);
        sigil_save_unit_free(u);
    }
}

/* bsnes writes a clock cart's time.rtc and the Satellaview cart's download
 * RAM beside the .srm; both travel when present and neither is expected. */
/* ---- N64 standalone emulators ---------------------------------------------- */

#define N64_MD5     "FA27089C425DBAB99F19245C5C997613"
#define N64_MD5_N64 "10C93DD78B695CD32B6938534ED0EDD5"
#define FZ_GAME     "GameData/1080 SNOWBOARDING (JU) fa27089c425dbab99f19245c5c997613/"
#define PJ64_GAME   "Save/1080 SNOWBOARDING-" N64_MD5_N64 "/"

static void expect_n64(const char *what, const char *layout, uint32_t version, const char *header,
                       const sigil_save_option *opts, size_t opt_count, const char *const *listing, size_t listing_count,
                       const char *const *want, size_t want_count) {
    sigil_result result;
    memset(&result, 0, sizeof(result));
    result.struct_version = version;
    result.platform = SIGIL_PLATFORM_N64;
    snprintf(result.title_id, sizeof(result.title_id), "NTEA");
    snprintf(result.n64_header, sizeof(result.n64_header), "%s", header);
    snprintf(result.n64_md5, sizeof(result.n64_md5), "%s", N64_MD5);
    snprintf(result.n64_md5_n64, sizeof(result.n64_md5_n64), "%s", N64_MD5_N64);
    sigil_save_request req;
    memset(&req, 0, sizeof(req));
    req.struct_version = SIGIL_SAVE_REQUEST_V1;
    req.layout = layout;
    req.platform = "n64";
    req.content_path = "1080 Snowboarding (Japan, USA) (En,Ja).z64";
    req.result = &result;
    req.options = opts;
    req.option_count = opt_count;
    req.listing = listing;
    req.listing_count = listing_count;
    sigil_save_unit *u = NULL;
    if (sigil_save_resolve(&req, &u) != SIGIL_OK || !u) {
        fail(what, "resolve failed");
        return;
    }
    bool same = u->member_count == want_count;
    for (size_t i = 0; same && i < want_count; i++) same = strcmp(u->members[i].path, want[i]) == 0;
    if (!same) {
        for (size_t i = 0; i < u->member_count; i++) fprintf(stderr, "  member %s\n", u->members[i].path);
        fail(what, "members differ from the files the emulator keeps for the game");
    }
    sigil_save_unit_free(u);
}

static void test_n64_standalone(void) {
    const char *mupen[] = {
        "1080 Snowboarding (JU) [!]-FA27089C.eep", "1080 SNOWBOARDING-FA27089C.mpk", "Other Game-12345678.eep",
        "1080 Snowboarding (JU) [!]-FA27089D.eep", "sub/1080-FA27089C.eep", "1080 Snowboarding (JU) [!].eep",
    };
    const char *mupen_want[] = { "1080 Snowboarding (JU) [!]-FA27089C.eep", "1080 SNOWBOARDING-FA27089C.mpk" };
    expect_n64("mupen64plus by the hash suffix", "mupen64plus_standalone", SIGIL_RESULT_V4, "1080 SNOWBOARDING", NULL, 0,
               mupen, 6, mupen_want, 2);

    const char *fz[] = {
        FZ_GAME "SramData/1080 Snowboarding (JU) [!].eep",
        FZ_GAME "SramData/1080 Snowboarding (JU) [!].mpk",
        FZ_GAME "AutoSaves/2026-10-05.sav",
        "GameData/fa27089c425dbab99f19245c5c997613/SramData/1080 Snowboarding (JU) [!].sra",
        "GameData/OTHER GAME (U) 00000000000000000000000000000000/SramData/Other.eep",
        "GameData/1080 Snowboarding (JU) [!].eep",
    };
    const char *fz_want[] = {
        FZ_GAME "SramData/1080 Snowboarding (JU) [!].eep",
        "GameData/fa27089c425dbab99f19245c5c997613/SramData/1080 Snowboarding (JU) [!].sra",
        FZ_GAME "SramData/1080 Snowboarding (JU) [!].mpk",
    };
    expect_n64("m64plus fz by the folder's hash", "m64plus_fz", SIGIL_RESULT_V4, "1080 SNOWBOARDING", NULL, 0, fz, 6,
               fz_want, 3);

    const char *pj64[] = {
        PJ64_GAME "1080 SNOWBOARDING.eep", PJ64_GAME "1080 SNOWBOARDING_Cont_1.mpk",
        PJ64_GAME "1080 SNOWBOARDING_Cont_3.mpk", "Save/1080 SNOWBOARDING.eep",
        "Save/1080 SNOWBOARDING-" N64_MD5 "/1080 SNOWBOARDING.eep",
    };
    const char *pj64_want[] = {
        PJ64_GAME "1080 SNOWBOARDING.eep", PJ64_GAME "1080 SNOWBOARDING_Cont_1.mpk",
        PJ64_GAME "1080 SNOWBOARDING_Cont_3.mpk",
    };
    expect_n64("project64 unique game dir", "project64", SIGIL_RESULT_V4, "1080 SNOWBOARDING", NULL, 0, pj64, 5,
               pj64_want, 3);
    expect_n64("project64 unique game dir, katakana header", "project64", SIGIL_RESULT_V4, "", NULL, 0, pj64, 5,
               pj64_want, 3);

    const sigil_save_option flat[] = { { "Unique Game Dir", "0" } };
    const char *pj64_flat[] = {
        "Save/1080 SNOWBOARDING.eep", "Save/1080 SNOWBOARDING_Cont_2.mpk", PJ64_GAME "1080 SNOWBOARDING.eep",
        "Save/1080 SNOWBOARDING 2.eep",
    };
    const char *pj64_flat_want[] = { "Save/1080 SNOWBOARDING.eep", "Save/1080 SNOWBOARDING_Cont_2.mpk" };
    expect_n64("project64 flat", "project64", SIGIL_RESULT_V4, "1080 SNOWBOARDING", flat, 1, pj64_flat, 4,
               pj64_flat_want, 2);
    expect_n64("project64 flat, katakana header", "project64", SIGIL_RESULT_V4, "", flat, 1, pj64_flat, 4, NULL, 0);

    expect_n64("a result from before the N64 fields", "mupen64plus_standalone", SIGIL_RESULT_V3, "1080 SNOWBOARDING",
               NULL, 0, mupen, 6, NULL, 0);
}

static void test_bsnes_files(void) {
    const char *all[] = { "G.srm", "G.rtc", "G.psr" };
    sigil_save_unit *u = resolve("bsnes all", "bsnes", "snes", "G.sfc", 0, NULL, 0, all, 3, NULL);
    if (u) {
        const char *want[] = { "G.srm", "G.rtc", "G.psr" };
        expect_members("bsnes all", u, SIGIL_SAVE_SHAPE_MULTI, want, 3);
        if (u->member_count == 3 && (u->members[0].role != SIGIL_SAVE_ROLE_PRIMARY ||
                                     u->members[1].role != SIGIL_SAVE_ROLE_RTC ||
                                     u->members[2].role != SIGIL_SAVE_ROLE_SIDECAR)) {
            fail("bsnes all", "roles");
        }
        sigil_save_unit_free(u);
    }
    const char *srm[] = { "G.srm" };
    u = resolve("bsnes srm alone", "bsnes", "snes", "G.sfc", 0, NULL, 0, srm, 1, NULL);
    if (u) {
        expect_members("bsnes srm alone", u, SIGIL_SAVE_SHAPE_SINGLE, srm, 1);
        if (u->expected_count != 0) fail("bsnes srm alone", "expects a clock or download file");
        sigil_save_unit_free(u);
    }
    u = resolve("bsnes clock cart", "bsnes", "snes", "G.sfc", SIGIL_FEATURE_RTC, NULL, 0, srm, 1, NULL);
    if (u) {
        if (u->expected_count != 1 || strcmp(u->expected[0].path, "G.rtc") != 0) {
            fail("bsnes clock cart", "a clock cart doesn't expect its .rtc");
        }
        sigil_save_unit_free(u);
    }
}

/* Nestopia keeps a disk's save as a patch the core writes, and no .srm: the
 * patch is the primary, so locate expects it, and fceumm's .srm stays out. */
static void test_fds_patch(void) {
    const char *switched[] = { "G.srm", "G.sav" };
    sigil_save_unit *u = resolve("fds nothing yet", "nestopia", "fds", "G.fds", 0, NULL, 0, switched, 0, NULL);
    if (u) {
        if (u->expected_count != 1 || strcmp(u->expected[0].path, "G.sav") != 0 ||
            u->expected[0].role != SIGIL_SAVE_ROLE_PRIMARY) {
            fail("fds nothing yet", "the patch isn't the expected primary");
        }
        sigil_save_unit_free(u);
    }
    u = resolve("fds after fceumm", "nestopia", "fds", "G.fds", 0, NULL, 0, switched, 2, NULL);
    if (u) {
        const char *want[] = { "G.sav" };
        expect_members("fds after fceumm", u, SIGIL_SAVE_SHAPE_SINGLE, want, 1);
        if (u->members[0].role != SIGIL_SAVE_ROLE_PRIMARY) fail("fds after fceumm", "the patch isn't primary");
        expect_alternates("fds after fceumm", u, NULL, 0);
        sigil_save_unit_free(u);
    }
}

int main(void) {
    test_stem();
    test_default_layout();
    test_segacd();
    test_segacd_regions();
    test_segacd_cart_sizes();
    test_gamecube_targets();
    test_option_values();
    test_vmu_ports();
    test_psx_saturn();
    test_single_file_cores();
    test_arcade();
    test_hashes();
    test_alternates();
    test_fds_patch();
    test_bsnes_files();
    test_n64_standalone();

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_save_unit: ok\n");
    return 0;
}

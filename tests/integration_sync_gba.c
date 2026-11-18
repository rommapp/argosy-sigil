// SPDX-License-Identifier: MPL-2.0
/* GBA sync against saves real emulators wrote: one save in two emulators'
 * files gives one unit, restore writes each emulator's file byte for byte,
 * and standalone mGBA's clock travels as clock.rtc. The games each sample
 * shows on screen are in the manifest. */
#include "save_corpus.h"
#include "legacy_units.h"

#define TEST_SKIP 77
#define EEPROM  SIGIL_FEATURE_GBA_EEPROM
#define SRAM    SIGIL_FEATURE_GBA_SRAM
#define FLASH   SIGIL_FEATURE_GBA_FLASH
#define FLASH1M SIGIL_FEATURE_GBA_FLASH1M
#define RTC     SIGIL_FEATURE_RTC

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_gba;

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *content, uint32_t features) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_GBA;
    g->result.features = features;
    g->ids[0] = content;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "gba";
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

/* The unit `layout` collects from `data`, saved as `file`. */
static sigil_sync_result *collect(const char *layout, const char *content, const char *file, const uint8_t *data,
                                  size_t len, uint32_t features) {
    mem_root root = {0};
    root_put(&root, file, data, len);
    game g;
    make_game(&g, &root, layout, content, features);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        r = NULL;
    }
    root_free(&root);
    return r;
}

/* Restores `unit` for `layout` into `root`; the file it wrote, or NULL. */
static mem_file *restore(mem_root *root, const char *layout, const char *content, const char *file,
                         const uint8_t *unit, size_t len, uint32_t features) {
    game g;
    make_game(&g, root, layout, content, features);
    sigil_sync_result *w = NULL;
    int rc = sigil_restore(&g.req, unit, len, &w);
    sigil_sync_result_free(w);
    return rc == SIGIL_OK ? root_find(root, file) : NULL;
}

static bool md5_is(const uint8_t *data, size_t len, const char *want) {
    char md5[33];
    sigil_md5_of(data, len, md5);
    return strcmp(md5, want) == 0;
}

/* The clock footers the manifest rows describe: BCD date and time, control, lastLatch. */
static const uint8_t EMERALD_CLOCK[16] = { 0x25, 0x09, 0x28, 0x00, 0x19, 0x56, 0x58, 0x40,
                                           0xfa, 0x84, 0xd9, 0x68, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t BOKTAI_CLOCK[16] = { 0x26, 0x11, 0x18, 0x03, 0x02, 0x25, 0x56, 0x40,
                                          0xb4, 0x0c, 0xfd, 0x6a, 0x00, 0x00, 0x00, 0x00 };

/* An emulator's file: collected, the unit is the chip `want_md5` names; restored
 * for the same emulator, it is the emulator's file again, byte for byte. */
typedef struct {
    const char *id;
    const char *layout;
    const char *content;
    uint32_t    features;
    size_t      chip_len;
    const char *chip_md5;
} sample;

static const sample SAMPLES[] = {
    { "alien-hominid-mgba", "mgba", "AlienHominid.gba", EEPROM, 512, "968500e5e0cf25ddb539b64d7988b401" },
    { "alien-hominid-vba-next", "vba_next", "AlienHominid.gba", EEPROM, 512, "4704095c0f79b663b5a776985268329b" },
    { "astro-mgba-console", "mgba", "Astro Boy - Omega Factor (Cabinet).gba", EEPROM, 8192, "f5034e6921cce9495c9272edde48ca91" },
    { "astro-vba-next", "vba_next", "AstroBoy.gba", EEPROM, 8192, "4d78aaedba2c563a1930eeab0ed0cbdc" },
    { "astro-mgba-standalone", "mgba_standalone", "AstroBoy.gba", EEPROM, 8192, "4d78aaedba2c563a1930eeab0ed0cbdc" },
    { "fzero-mgba", "mgba", "FZero.gba", SRAM, 32768, "ac038aad2ed13b81a1c90e51284b8d37" },
    { "fzero-gpsp", "gpsp", "FZero.gba", SRAM, 32768, "222d38c6fb46f48e00938a2a4c17a5d6" },
    { "sa2-mgba-console", "mgba", "Sonic Advance 2 (Cabinet).gba", FLASH, 65536, "e732fe64c6638313178ce83cca0f4a59" },
    { "sa2-vba-next", "vba_next", "SonicAdvance2.gba", FLASH, 65536, "134ec4cdf67c3544d976f56a8ec8fb39" },
};

static void check_sample(const sample *s) {
    size_t len = 0;
    char file[SIGIL_SAVE_ENTRY_MAX];
    uint8_t *data = corpus_sample(&g_gba, "gba", s->id, NULL, &len);
    if (!data) return;
    int r = corpus_row_of(&g_gba, s->id, NULL);
    snprintf(file, sizeof(file), "%s", corpus_get(&g_gba, (size_t)r, "path"));
    sigil_sync_result *u = collect(s->layout, s->content, file, data, len, s->features);
    if (!u || u->len != s->chip_len || !md5_is(u->data, u->len, s->chip_md5) || strcmp(u->identity_hash, s->chip_md5) != 0) {
        fail(s->id, "the unit isn't the chip the emulator's run showed");
    } else {
        mem_root root = {0};
        mem_file *f = restore(&root, s->layout, s->content, file, u->data, u->len, s->features);
        if (!f || f->len != len || memcmp(f->data, data, len) != 0) fail(s->id, "restore isn't the emulator's own file");
        root_free(&root);
    }
    sigil_sync_result_free(u);
    free(data);
}

/* VBA Next's own file from no save holds the EEPROM at 128 KiB; without the ROM's facts it is found there. */
static void check_vba_next_new(void) {
    size_t len = 0;
    uint8_t *data = corpus_sample(&g_gba, "gba", "astro-vba-next-new", NULL, &len);
    if (!data) return;
    for (int named = 0; named < 2; named++) {
        sigil_sync_result *u = collect("vba_next", "AstroBoy.gba", "AstroBoy.srm", data, len, named ? EEPROM : 0);
        if (!u || u->len != 8192 || len != 0x22000 || memcmp(u->data, data + 0x20000, 8192) != 0) {
            fail("astro-vba-next-new", named ? "not the EEPROM at 128 KiB" : "not the EEPROM at 128 KiB without the ROM's facts");
        } else {
            mem_root root = {0};
            mem_file *f = restore(&root, "vba_next", "AstroBoy.gba", "AstroBoy.srm", u->data, u->len, EEPROM);
            if (!f || f->len != len || memcmp(f->data, data, len) != 0) fail("astro-vba-next-new", "restore isn't VBA Next's file");
            root_free(&root);
        }
        sigil_sync_result_free(u);
    }
    free(data);
}

/* Standalone mGBA grew a 512-byte EEPROM file of an 8 KiB EEPROM game with
 * zeros: they are bytes of the chip, so the unit keeps 8 KiB. */
static void check_zerofill(void) {
    size_t len = 0;
    uint8_t *data = corpus_sample(&g_gba, "gba", "sma2-mgba-standalone-zerofill", NULL, &len);
    if (!data) return;
    sigil_sync_result *u = collect("mgba_standalone", "SMA2.gba", "SMA2.sav", data, len, EEPROM);
    if (!u || u->len != 8192 || memcmp(u->data, data, 8192) != 0) fail("sma2-mgba-standalone-zerofill", "zeros taken for padding");
    sigil_sync_result_free(u);
    free(data);
}

static uint8_t *member_of(const sigil_sync_result *r, const char *name, size_t *len) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    uint8_t *out = NULL;
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) != SIGIL_OK) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, name) != 0) continue;
        out = (uint8_t *)malloc(m[i].len ? m[i].len : 1);
        memcpy(out, m[i].data, m[i].len);
        *len = m[i].len;
    }
    sigil_zip_members_free(m, n);
    return out;
}

/* Standalone mGBA's clock: the footer travels as clock.rtc and comes back
 * byte for byte; libretro mGBA's save of the same game comes back into the
 * standalone folder with the clock that was there. */
static void check_clock(void) {
    const char *E = "Pokemon - Emerald Version (USA, Europe).gba";
    size_t len = 0, mgba_len = 0;
    uint8_t *sav = corpus_sample(&g_gba, "gba", "emerald-mgba-rtc-footer", NULL, &len);
    uint8_t *srm = corpus_sample(&g_gba, "gba", "emerald-mgba", NULL, &mgba_len);
    if (!sav || !srm || len != 131072 + 16) {
        free(sav);
        free(srm);
        return;
    }
    sigil_sync_result *u = collect("mgba_standalone", E, "Pokemon - Emerald Version (USA, Europe).sav", sav, len,
                                   FLASH1M | RTC);
    size_t ram_len = 0, rtc_len = 0;
    uint8_t *ram = member_of(u, "save.sram", &ram_len);
    uint8_t *rtc = member_of(u, "clock.rtc", &rtc_len);
    if (!ram || ram_len != 131072 || !md5_is(ram, ram_len, "b45d5d80feba98a437463fc49804bfd7") || !rtc ||
        rtc_len != 16 || memcmp(rtc, EMERALD_CLOCK, 16) != 0) {
        fail("emerald-mgba-rtc-footer", "not save.sram and the footer as clock.rtc");
    } else {
        mem_root empty = {0}, libretro = {0};
        mem_file *f = restore(&empty, "mgba_standalone", E, "Pokemon - Emerald Version (USA, Europe).sav", u->data,
                              u->len, FLASH1M | RTC);
        if (!f || f->len != len || memcmp(f->data, sav, len) != 0) fail("emerald-mgba-rtc-footer", "standalone file not back byte for byte");
        f = restore(&libretro, "mgba", E, "Pokemon - Emerald Version (USA, Europe).srm", u->data, u->len, FLASH1M | RTC);
        if (!f || f->len != 131072 || memcmp(f->data, sav, 131072) != 0 || libretro.count != 1) {
            fail("emerald-mgba-rtc-footer", "libretro mGBA doesn't get the flash alone");
        }
        root_free(&empty);
        root_free(&libretro);
    }

    sigil_sync_result *back = collect("mgba", E, "Pokemon - Emerald Version (USA, Europe).srm", srm, mgba_len, FLASH1M | RTC);
    uint8_t *clock = member_of(back, "clock.rtc", &rtc_len);
    mem_root folder = {0};
    root_put(&folder, "Pokemon - Emerald Version (USA, Europe).sav", sav, len);
    mem_file *f = back ? restore(&folder, "mgba_standalone", E, "Pokemon - Emerald Version (USA, Europe).sav", back->data,
                                 back->len, FLASH1M | RTC)
                       : NULL;
    if (!back || clock || back->shape != SIGIL_SAVE_SHAPE_MULTI || !f || f->len != len ||
        memcmp(f->data, srm, 131072) != 0 || memcmp(f->data + 131072, sav + 131072, 16) != 0) {
        fail("emerald-mgba", "the standalone file isn't libretro mGBA's save with the clock that was there");
    }
    root_free(&folder);
    free(clock);
    sigil_sync_result_free(back);
    free(ram);
    free(rtc);
    sigil_sync_result_free(u);
    free(sav);
    free(srm);
}

/* An EEPROM clock cart: standalone mGBA's .sav is the 8 KiB EEPROM and the
 * footer, libretro mGBA's the EEPROM alone; each comes back as its
 * emulator keeps it, and libretro mGBA's save keeps the clock standalone
 * mGBA had. */
static void check_eeprom_clock(void) {
    size_t len = 0, mgba_len = 0;
    uint8_t *sav = corpus_sample(&g_gba, "gba", "boktai-mgba-standalone", NULL, &len);
    uint8_t *srm = corpus_sample(&g_gba, "gba", "boktai-mgba", NULL, &mgba_len);
    if (!sav || !srm || len != 8192 + 16 || mgba_len != 8192) {
        free(sav);
        free(srm);
        return;
    }
    sigil_sync_result *u = collect("mgba_standalone", "Boktai.gba", "Boktai.sav", sav, len, EEPROM | RTC);
    size_t ram_len = 0, rtc_len = 0;
    uint8_t *ram = member_of(u, "save.sram", &ram_len);
    uint8_t *rtc = member_of(u, "clock.rtc", &rtc_len);
    if (!ram || ram_len != 8192 || !md5_is(ram, ram_len, "d60ddfbb8a37281013a292b7d62d1ea3") || !rtc || rtc_len != 16 ||
        memcmp(rtc, BOKTAI_CLOCK, 16) != 0) {
        fail("boktai-mgba-standalone", "not the EEPROM and the footer as clock.rtc");
    } else {
        mem_root empty = {0}, libretro = {0}, vba = {0};
        mem_file *f = restore(&empty, "mgba_standalone", "Boktai.gba", "Boktai.sav", u->data, u->len, EEPROM | RTC);
        if (!f || f->len != len || memcmp(f->data, sav, len) != 0) fail("boktai-mgba-standalone", "standalone file not back byte for byte");
        f = restore(&libretro, "mgba", "Boktai.gba", "Boktai.srm", u->data, u->len, EEPROM | RTC);
        if (!f || f->len != 8192 || memcmp(f->data, sav, 8192) != 0 || libretro.count != 1) {
            fail("boktai-mgba-standalone", "libretro mGBA doesn't get the EEPROM alone");
        }
        f = restore(&vba, "vba_next", "Boktai.gba", "Boktai.srm", u->data, u->len, EEPROM | RTC);
        if (!f || f->len != 0x22000 || memcmp(f->data + 0x20000, sav, 8192) != 0) fail("boktai-mgba-standalone", "VBA Next's EEPROM isn't at 128 KiB");
        root_free(&empty);
        root_free(&libretro);
        root_free(&vba);
    }
    sigil_sync_result *back = collect("mgba", "Boktai.gba", "Boktai.srm", srm, mgba_len, EEPROM | RTC);
    mem_root folder = {0};
    root_put(&folder, "Boktai.sav", sav, len);
    mem_file *f = back ? restore(&folder, "mgba_standalone", "Boktai.gba", "Boktai.sav", back->data, back->len, EEPROM | RTC) : NULL;
    if (!f || f->len != len || memcmp(f->data, srm, 8192) != 0 || memcmp(f->data + 8192, sav + 8192, 16) != 0) {
        fail("boktai-mgba", "the standalone file isn't libretro mGBA's save with the clock that was there");
    }
    root_free(&folder);
    sigil_sync_result_free(back);
    sigil_sync_result_free(u);
    free(ram);
    free(rtc);
    free(sav);
    free(srm);
}

/* The maintainer's samples, where present: the manifest's notes give each
 * pair's expected bytes. They run only on a machine that has those files. */
static void check_store_pairs(void) {
    static const struct { const char *padded, *exact, *content; uint32_t features; } PAIRS[] = {
        { "dbaa-gpsp", "dbaa-retroarch", "Game.gba", EEPROM },
        { "mzm-builtin-128k", "mzm-builtin-32k", "Game.gba", SRAM },
    };
    for (size_t i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); i++) {
        if (!corpus_present(&g_gba, "gba", PAIRS[i].padded) || !corpus_present(&g_gba, "gba", PAIRS[i].exact)) continue;
        size_t plen = 0, elen = 0;
        uint8_t *padded = corpus_sample(&g_gba, "gba", PAIRS[i].padded, NULL, &plen);
        uint8_t *exact = corpus_sample(&g_gba, "gba", PAIRS[i].exact, NULL, &elen);
        sigil_sync_result *a = collect("gpsp", PAIRS[i].content, "Game.srm", padded, plen, PAIRS[i].features);
        sigil_sync_result *b = collect("mgba", PAIRS[i].content, "Game.srm", exact, elen, PAIRS[i].features);
        if (!a || !b || strcmp(a->identity_hash, b->identity_hash) != 0 || a->len != elen) {
            fail(PAIRS[i].padded, "the padded file isn't the exact one's unit");
        }
        sigil_sync_result_free(a);
        sigil_sync_result_free(b);
        free(padded);
        free(exact);
    }
    static const struct { const char *id; uint32_t features; size_t len; } KEPT[] = {
        { "advance-wars-retroarch", FLASH, 65536 },
        { "azul-agua-mgba", FLASH1M, 131072 },
    };
    for (size_t i = 0; i < sizeof(KEPT) / sizeof(KEPT[0]); i++) {
        if (!corpus_present(&g_gba, "gba", KEPT[i].id)) continue;
        size_t len = 0;
        uint8_t *data = corpus_sample(&g_gba, "gba", KEPT[i].id, NULL, &len);
        for (int named = 0; named < 2; named++) {
            sigil_sync_result *u = collect("mgba", "Game.gba", "Game.srm", data, len, named ? KEPT[i].features : 0);
            if (!u || u->len != KEPT[i].len || memcmp(u->data, data, len) != 0) fail(KEPT[i].id, "shortened");
            sigil_sync_result_free(u);
        }
        free(data);
    }
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("gba", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_gba) != 0) {
        fprintf(stderr, "SKIP: no gba manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_gba, "gba", "astro-vba-next")) {
        fprintf(stderr, "SKIP: gba emulator-run samples missing\n");
        corpus_free(&g_gba);
        return TEST_SKIP;
    }
    for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) check_sample(&SAMPLES[i]);
    check_vba_next_new();
    check_zerofill();
    check_clock();
    check_eeprom_clock();
    check_store_pairs();
    corpus_free(&g_gba);
    printf("gba sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}

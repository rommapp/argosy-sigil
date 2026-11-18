// SPDX-License-Identifier: MPL-2.0
/* GBA saves through collect and restore: every emulator's file of each save
 * chip gives one unit, restore writes each emulator's own file, and a file
 * from one emulator restores into another's. The forms come from the
 * emulators' sources: mGBA's libretro.c and gba/savedata.c, gpSP's
 * libretro.c, VBA Next's libretro.c and memory.c. */
#include "legacy_units.h"
#include <stdio.h>

#define K 1024u
#define VBA_NEXT_FILE 0x22000u

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static bool all_ff_tail(const uint8_t *p, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, uint32_t features) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_GBA;
    g->result.features = features;
    g->ids[0] = "Game";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = "gba";
    g->req.save.content_path = "Game.gba";
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

/* One save of each chip, as the unit holds it. */
typedef struct {
    const char *name;
    uint32_t    feature;
    size_t      len;
    bool        eeprom;
    uint8_t     bytes[128 * 1024];
} chip;

static chip g_chips[5];
static uint8_t g_clock[16] = { 0x25, 0x09, 0x28, 0x00, 0x19, 0x56, 0x58, 0x40,
                               0xFA, 0x50, 0xD9, 0x68, 0x00, 0x00, 0x00, 0x00 };

static void make_chips(void) {
    static const struct { const char *name; uint32_t feature; size_t len; bool eeprom; } C[] = {
        { "eeprom 4 Kbit", SIGIL_FEATURE_GBA_EEPROM, 512, true },
        { "eeprom 64 Kbit", SIGIL_FEATURE_GBA_EEPROM, 8 * K, true },
        { "sram", SIGIL_FEATURE_GBA_SRAM, 32 * K, false },
        { "flash 64 KiB", SIGIL_FEATURE_GBA_FLASH, 64 * K, false },
        { "flash 128 KiB", SIGIL_FEATURE_GBA_FLASH1M, 128 * K, false },
    };
    for (size_t c = 0; c < 5; c++) {
        chip *x = &g_chips[c];
        x->name = C[c].name;
        x->feature = C[c].feature;
        x->len = C[c].len;
        x->eeprom = C[c].eeprom;
        memset(x->bytes, 0xFF, sizeof(x->bytes));
        for (size_t i = 0; i < x->len; i++) x->bytes[i] = (uint8_t)(i * 31 + c);
        /* A flash save's first half erased and its last byte set, as Advance
         * Wars's: trimming trailing 0xFF must not shorten it. */
        if (x->len == 64 * K) memset(x->bytes, 0xFF, 64 * K - 1);
    }
}

/* What each emulator writes for `c`. */
typedef enum { F_MGBA, F_VBAM, F_GPSP, F_VBA_NEXT, F_MGBA_STANDALONE, F_COUNT } form;
static const char *const LAYOUTS[F_COUNT] = { "mgba", "vbam", "gpsp", "vba_next", "mgba_standalone" };
static const char *const FILES[F_COUNT] = { "Game.srm", "Game.srm", "Game.srm", "Game.srm", "Game.sav" };

static size_t form_of(const chip *c, form f, bool clock, uint8_t *out) {
    size_t len = c->len;
    memset(out, 0xFF, VBA_NEXT_FILE + 16);
    switch (f) {
    case F_MGBA:
    case F_VBAM:
        memcpy(out, c->bytes, c->len);
        break;
    case F_GPSP:
        memcpy(out, c->bytes, c->len);
        len = 128 * K;
        break;
    case F_VBA_NEXT:
        memcpy(out + (c->eeprom ? 0x20000 : 0), c->bytes, c->len);
        len = VBA_NEXT_FILE;
        break;
    case F_MGBA_STANDALONE:
        memcpy(out, c->bytes, c->len);
        if (clock) {
            memcpy(out + len, g_clock, 16);
            len += 16;
        }
        break;
    default:
        break;
    }
    return len;
}

static sigil_sync_result *collect_file(const char *layout, const char *file, const uint8_t *data, size_t len,
                                       uint32_t features, int *rc) {
    mem_root root = {0};
    root_put(&root, file, data, len);
    game g;
    make_game(&g, &root, layout, features);
    sigil_sync_result *r = NULL;
    *rc = sigil_collect(&g.req, &r);
    root_free(&root);
    return r;
}

static uint8_t g_file[VBA_NEXT_FILE + 16], g_want[VBA_NEXT_FILE + 16];

/* Every emulator's file collects to the chip's bytes, with the ROM's facts and without. */
static void check_collect(void) {
    for (size_t c = 0; c < 5; c++) {
        const chip *x = &g_chips[c];
        for (int f = 0; f < F_COUNT; f++) {
            for (int named = 1; named >= 0; named--) {
                char where[96];
                snprintf(where, sizeof(where), "collect %s from %s%s", x->name, LAYOUTS[f], named ? "" : ", no facts");
                size_t len = form_of(x, (form)f, false, g_file);
                int rc;
                sigil_sync_result *r = collect_file(LAYOUTS[f], FILES[f], g_file, len, named ? x->feature : 0, &rc);
                /* Without the ROM's facts, a padded file keeps its length:
                 * nothing in it says which chip it holds. VBA Next's EEPROM is
                 * told apart by where it sits. */
                bool padded = !named && f == F_GPSP && x->len != 128 * K;
                bool vba_ram = !named && f == F_VBA_NEXT && !x->eeprom && x->len != 128 * K;
                size_t want = padded || vba_ram ? 128 * K : x->len;
                if (rc != SIGIL_OK || !r || r->len != want || memcmp(r->data, x->bytes, x->len) != 0 ||
                    r->shape != SIGIL_SAVE_SHAPE_SINGLE || strcmp(r->artifact, "Game.srm") != 0) {
                    fail(where, "not the chip's bytes");
                } else if (want > x->len && !all_ff_tail(r->data, x->len, want)) {
                    fail(where, "padding isn't 0xFF");
                }
                sigil_sync_result_free(r);
            }
        }
    }
}

/* Restore writes each emulator's own file, from sigil's unit and from every other emulator's file. */
static void check_restore(void) {
    for (size_t c = 0; c < 5; c++) {
        const chip *x = &g_chips[c];
        for (int from = -1; from < F_COUNT; from++) {
            const uint8_t *unit = x->bytes;
            size_t unit_len = x->len;
            uint8_t source[VBA_NEXT_FILE + 16];
            if (from >= 0) {
                unit_len = form_of(x, (form)from, false, source);
                unit = source;
            }
            for (int to = 0; to < F_COUNT; to++) {
                char where[128];
                snprintf(where, sizeof(where), "restore %s from %s into %s", x->name,
                         from < 0 ? "the unit" : LAYOUTS[from], LAYOUTS[to]);
                mem_root root = {0};
                game g;
                make_game(&g, &root, LAYOUTS[to], x->feature);
                sigil_sync_result *w = NULL;
                mem_file *f = NULL;
                size_t want = form_of(x, (form)to, false, g_want);
                int rc = sigil_restore(&g.req, unit, unit_len, &w);
                if (rc != SIGIL_OK || !(f = root_find(&root, FILES[to])) || root.count != 1) {
                    fail(where, "no file");
                } else if (f->len != want || memcmp(f->data, g_want, want) != 0) {
                    fail(where, "not the emulator's form");
                }
                sigil_sync_result_free(w);
                root_free(&root);
            }
        }
    }
}

/* EEPROM: a file that is exactly 512 bytes or 8 KiB keeps its size; from a
 * padded file it is 512 bytes unless a byte past them isn't 0xFF. Each
 * emulator's file comes back from a round trip the same size, byte for byte. */
static void check_eeprom_size(void) {
    uint8_t small[8 * K];
    memset(small, 0xFF, sizeof(small));
    memcpy(small, g_chips[0].bytes, 512);
    for (int named = 1; named >= 0; named--) {
        int rc;
        sigil_sync_result *r = collect_file("vbam", "Game.srm", small, sizeof(small), named ? SIGIL_FEATURE_GBA_EEPROM : 0, &rc);
        if (rc != SIGIL_OK || !r || r->len != 8 * K || memcmp(r->data, small, sizeof(small)) != 0) {
            fail(named ? "exact 8 KiB EEPROM file, nothing past 512" : "exact 8 KiB file, no facts", "not kept at 8 KiB");
        }
        sigil_sync_result_free(r);
    }
    uint8_t padded[128 * K];
    memset(padded, 0xFF, sizeof(padded));
    memcpy(padded, g_chips[0].bytes, 512);
    int rc;
    sigil_sync_result *r = collect_file("gpsp", "Game.srm", padded, sizeof(padded), SIGIL_FEATURE_GBA_EEPROM, &rc);
    if (rc != SIGIL_OK || !r || r->len != 512 || memcmp(r->data, padded, 512) != 0) {
        fail("gpSP's EEPROM file, nothing past 512", "not 512 bytes");
    }
    sigil_sync_result_free(r);

    /* The 8 KiB file round-trips at 8 KiB in each emulator that writes one. */
    static const struct { const char *layout, *file; } EXACT[] = {
        { "mgba", "Game.srm" }, { "vbam", "Game.srm" }, { "mgba_standalone", "Game.sav" },
    };
    for (size_t i = 0; i < sizeof(EXACT) / sizeof(EXACT[0]); i++) {
        r = collect_file(EXACT[i].layout, EXACT[i].file, small, sizeof(small), SIGIL_FEATURE_GBA_EEPROM, &rc);
        mem_root root = {0};
        game g;
        make_game(&g, &root, EXACT[i].layout, SIGIL_FEATURE_GBA_EEPROM);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        if (!r || sigil_restore(&g.req, r->data, r->len, &w) != SIGIL_OK || !(f = root_find(&root, EXACT[i].file)) ||
            f->len != sizeof(small) || memcmp(f->data, small, sizeof(small)) != 0) {
            fail(EXACT[i].layout, "an 8 KiB EEPROM file doesn't round-trip at 8 KiB");
        }
        sigil_sync_result_free(w);
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

/* mGBA's libretro core in RetroArch leaves a 128 KiB file; restore over it
 * keeps that length, and into an empty folder writes the chip's size. */
static void check_retroarch_length(void) {
    const chip *x = &g_chips[2];
    uint8_t file[128 * K];
    memset(file, 0xFF, sizeof(file));
    memcpy(file, x->bytes, x->len);
    int rc;
    sigil_sync_result *r = collect_file("mgba", "Game.srm", file, sizeof(file), x->feature, &rc);
    if (rc != SIGIL_OK || !r || r->len != x->len) {
        fail("RetroArch's mGBA file", "not the chip");
        sigil_sync_result_free(r);
        return;
    }
    mem_root there = {0}, empty = {0};
    root_put(&there, "Game.srm", file, sizeof(file));
    game g;
    make_game(&g, &there, "mgba", x->feature);
    sigil_sync_result *w = NULL;
    mem_file *f = NULL;
    if (sigil_restore(&g.req, r->data, r->len, &w) != SIGIL_OK || !(f = root_find(&there, "Game.srm")) ||
        f->len != sizeof(file) || memcmp(f->data, file, sizeof(file)) != 0) {
        fail("RetroArch's mGBA file", "restore over it doesn't keep it byte for byte");
    }
    sigil_sync_result_free(w);
    make_game(&g, &empty, "mgba", x->feature);
    w = NULL;
    if (sigil_restore(&g.req, r->data, r->len, &w) != SIGIL_OK || !(f = root_find(&empty, "Game.srm")) || f->len != x->len) {
        fail("RetroArch's mGBA file", "an empty folder doesn't get the chip's size");
    }
    sigil_sync_result_free(w);
    sigil_sync_result_free(r);
    root_free(&there);
    root_free(&empty);
}

/* The ROM's facts never cost data: bytes past the chip they name keep the file at a size that holds them. */
static void check_never_drops(void) {
    uint8_t file[128 * K];
    memset(file, 0xFF, sizeof(file));
    memcpy(file, g_chips[3].bytes, 64 * K);
    file[100 * K] = 0x12;
    int rc;
    sigil_sync_result *r = collect_file("gpsp", "Game.srm", file, sizeof(file), SIGIL_FEATURE_GBA_FLASH, &rc);
    if (rc != SIGIL_OK || !r || r->len != 128 * K || memcmp(r->data, file, sizeof(file)) != 0) {
        fail("flash named 64 KiB with data past it", "data dropped");
    }
    sigil_sync_result_free(r);

    r = collect_file("gpsp", "Game.srm", file, sizeof(file), SIGIL_FEATURE_GBA_EEPROM, &rc);
    if (rc != SIGIL_OK || !r || r->len != 128 * K) fail("eeprom named with data past 8 KiB", "data dropped");
    sigil_sync_result_free(r);

    uint8_t big[200 * K];
    memset(big, 0, sizeof(big));
    r = collect_file("mgba", "Game.srm", big, sizeof(big), 0, &rc);
    if (rc != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("a file past 128 KiB", "taken as a save");
    sigil_sync_result_free(r);
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

/* A 128 KiB flash clock cart: standalone mGBA's footer travels as
 * clock.rtc, the identity covers the chip alone, and only standalone mGBA
 * gets the clock back: mGBA's libretro core has no room for it past the
 * flash in its 128 KiB save RAM. */
static void check_clock(void) {
    const chip *x = &g_chips[4];
    const uint32_t emerald = SIGIL_FEATURE_GBA_FLASH1M | SIGIL_FEATURE_RTC;
    size_t len = form_of(x, F_MGBA_STANDALONE, true, g_file);
    int rc;
    sigil_sync_result *timed = collect_file("mgba_standalone", "Game.sav", g_file, len, emerald, &rc);
    size_t ram_len = 0, rtc_len = 0;
    uint8_t *ram = member_of(timed, "save.sram", &ram_len);
    uint8_t *rtc = member_of(timed, "clock.rtc", &rtc_len);
    if (rc != SIGIL_OK || !timed || timed->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(timed->artifact, "Game.zip") != 0 ||
        !ram || ram_len != x->len || memcmp(ram, x->bytes, x->len) != 0 || !rtc || rtc_len != 16 ||
        memcmp(rtc, g_clock, 16) != 0) {
        fail("clock cart from standalone mGBA", "not save.sram and clock.rtc");
    }
    free(ram);
    free(rtc);

    len = form_of(x, F_GPSP, false, g_file);
    sigil_sync_result *plain = collect_file("gpsp", "Game.srm", g_file, len, emerald, &rc);
    rtc = member_of(plain, "clock.rtc", &rtc_len);
    if (rc != SIGIL_OK || !plain || plain->shape != SIGIL_SAVE_SHAPE_MULTI || rtc ||
        strcmp(plain->identity_hash, timed ? timed->identity_hash : "") != 0) {
        fail("clock cart from gpSP", "not save.sram alone with the same identity");
    }
    free(rtc);

    /* Without the ROM's facts the footer still marks a clock cart. */
    len = form_of(x, F_MGBA_STANDALONE, true, g_file);
    sigil_sync_result *unnamed = collect_file("mgba_standalone", "Game.sav", g_file, len, 0, &rc);
    if (rc != SIGIL_OK || !unnamed || unnamed->shape != SIGIL_SAVE_SHAPE_MULTI ||
        strcmp(unnamed->identity_hash, timed ? timed->identity_hash : "") != 0) {
        fail("footer, no facts", "not taken as a clock cart");
    }
    sigil_sync_result_free(unnamed);

    for (int to = 0; to < F_COUNT && timed; to++) {
        mem_root root = {0};
        game g;
        make_game(&g, &root, LAYOUTS[to], emerald);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        size_t want = form_of(x, (form)to, to == F_MGBA_STANDALONE, g_want);
        if (sigil_restore(&g.req, timed->data, timed->len, &w) != SIGIL_OK || !(f = root_find(&root, FILES[to])) ||
            root.count != 1 || f->len != want || memcmp(f->data, g_want, want) != 0) {
            fail(LAYOUTS[to], "clock cart restore isn't the emulator's form");
        }
        sigil_sync_result_free(w);
        root_free(&root);
    }

    /* A unit without the clock keeps the one standalone mGBA already has. */
    if (plain) {
        mem_root root = {0};
        uint8_t old[128 * K + 16];
        memset(old, 0x00, sizeof(old));
        memcpy(old + 128 * K, g_clock, 16);
        root_put(&root, "Game.sav", old, sizeof(old));
        game g;
        make_game(&g, &root, "mgba_standalone", emerald);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        if (sigil_restore(&g.req, plain->data, plain->len, &w) != SIGIL_OK || !(f = root_find(&root, "Game.sav")) ||
            f->len != 128 * K + 16 || memcmp(f->data, x->bytes, 128 * K) != 0 ||
            memcmp(f->data + 128 * K, g_clock, 16) != 0) {
            fail("unit without a clock into standalone mGBA", "the clock there isn't kept");
        }
        sigil_sync_result_free(w);
        root_free(&root);
    }
    sigil_sync_result_free(plain);
    sigil_sync_result_free(timed);
}

/* Unit `u` restored for `layout` into `root`: the file written, or NULL when
 * restore fails or leaves more than one file. */
static mem_file *restore_into(mem_root *root, const char *layout, const char *file, uint32_t features,
                              const sigil_sync_result *u) {
    game g;
    make_game(&g, root, layout, features);
    sigil_sync_result *w = NULL;
    int rc = u ? sigil_restore(&g.req, u->data, u->len, &w) : SIGIL_ERR_INVALID_ARG;
    sigil_sync_result_free(w);
    return rc == SIGIL_OK && root->count == 1 ? root_find(root, file) : NULL;
}

/* mGBA's libretro core keeps a clock cart's clock in its 128 KiB save RAM at
 * the chip's size, 8 KiB for an EEPROM, and RetroArch saves the whole buffer
 * (libretro/mgba at e31759b2: src/gba/savedata.c GBASavedataRTCWrite,
 * src/platform/libretro/libretro.c _doDeferredSetup). With the ROM naming
 * the chip and the clock, collect takes those 16 bytes as clock.rtc and the
 * chip alone as save.sram; restore writes the clock back there over such a
 * file, puts it after the chip for standalone mGBA, and leaves it out for
 * the others. */
static void check_libretro_clock(void) {
    for (size_t c = 0; c < 4; c++) {
        const chip *x = &g_chips[c];
        const uint32_t facts = x->feature | SIGIL_FEATURE_RTC;
        const size_t at = x->eeprom ? 8 * K : x->len;
        static uint8_t buffer[128 * K], bare[128 * K], want[128 * K + 16];
        memset(buffer, 0xFF, sizeof(buffer));
        memcpy(buffer, x->bytes, x->len);
        memcpy(bare, buffer, sizeof(bare));
        memcpy(buffer + at, g_clock, 16);
        char where[96];
        snprintf(where, sizeof(where), "libretro mGBA's clock, %s", x->name);

        int rc;
        sigil_sync_result *u = collect_file("mgba", "Game.srm", buffer, sizeof(buffer), facts, &rc);
        size_t ram_len = 0, rtc_len = 0;
        uint8_t *ram = member_of(u, "save.sram", &ram_len);
        uint8_t *rtc = member_of(u, "clock.rtc", &rtc_len);
        if (rc != SIGIL_OK || !ram || ram_len != x->len || memcmp(ram, x->bytes, x->len) != 0 || !rtc ||
            rtc_len != 16 || memcmp(rtc, g_clock, 16) != 0) {
            fail(where, "not the chip as save.sram and the clock as clock.rtc");
        }
        free(ram);
        free(rtc);
        size_t len = form_of(x, F_GPSP, false, g_file);
        sigil_sync_result *gpsp = collect_file("gpsp", "Game.srm", g_file, len, facts, &rc);
        if (!u || !gpsp || strcmp(u->identity_hash, gpsp->identity_hash) != 0) fail(where, "not gpSP's save's identity");
        sigil_sync_result_free(gpsp);

        mem_root over = {0}, without = {0}, empty = {0}, vba = {0}, standalone = {0};
        root_put(&over, "Game.srm", buffer, sizeof(buffer));
        mem_file *f = restore_into(&over, "mgba", "Game.srm", facts, u);
        if (!f || f->len != sizeof(buffer) || memcmp(f->data, buffer, sizeof(buffer)) != 0) {
            fail(where, "restore over libretro mGBA's file isn't the file back");
        }
        root_put(&without, "Game.srm", bare, sizeof(bare));
        f = restore_into(&without, "mgba", "Game.srm", facts, u);
        if (!f || f->len != sizeof(buffer) || memcmp(f->data, buffer, sizeof(buffer)) != 0) {
            fail(where, "the clock isn't written where the core keeps it");
        }
        f = restore_into(&empty, "mgba", "Game.srm", facts, u);
        if (!f || f->len != x->len || memcmp(f->data, x->bytes, x->len) != 0) fail(where, "an empty folder doesn't get the chip");
        len = form_of(x, F_VBA_NEXT, false, g_want);
        f = restore_into(&vba, "vba_next", "Game.srm", facts, u);
        if (!f || f->len != len || memcmp(f->data, g_want, len) != 0) fail(where, "not VBA Next's form");
        memset(want, 0xFF, sizeof(want));
        memcpy(want, x->bytes, x->len);
        memcpy(want + at, g_clock, 16);
        f = restore_into(&standalone, "mgba_standalone", "Game.sav", facts, u);
        if (!f || f->len != at + 16 || memcmp(f->data, want, at + 16) != 0) {
            fail(where, "standalone mGBA doesn't get the clock where it reads it");
        }
        root_free(&over);
        root_free(&without);
        root_free(&empty);
        root_free(&vba);
        root_free(&standalone);
        sigil_sync_result_free(u);
    }

    /* Without the ROM naming the chip and the clock, nothing says the 16
     * bytes are a clock: collect keeps them in the save. */
    const chip *x = &g_chips[1];
    static uint8_t buffer[128 * K];
    memset(buffer, 0xFF, sizeof(buffer));
    memcpy(buffer, x->bytes, x->len);
    memcpy(buffer + 8 * K, g_clock, 16);
    static const struct { uint32_t features; size_t len; const char *what; } UNNAMED[] = {
        { 0, 128 * K, "no facts" },
        { SIGIL_FEATURE_RTC, 128 * K, "the clock alone" },
        { SIGIL_FEATURE_GBA_EEPROM, 32 * K, "the chip alone" },
    };
    for (size_t i = 0; i < sizeof(UNNAMED) / sizeof(UNNAMED[0]); i++) {
        int rc;
        sigil_sync_result *u = collect_file("mgba", "Game.srm", buffer, sizeof(buffer), UNNAMED[i].features, &rc);
        uint8_t *ram = NULL;
        size_t ram_len = 0, rtc_len = 0;
        uint8_t *rtc = member_of(u, "clock.rtc", &rtc_len);
        if (u && u->shape == SIGIL_SAVE_SHAPE_MULTI) {
            ram = member_of(u, "save.sram", &ram_len);
        } else if (u) {
            ram_len = u->len;
            ram = (uint8_t *)malloc(ram_len);
            if (ram) memcpy(ram, u->data, ram_len);
        }
        if (rc != SIGIL_OK || rtc || !ram || ram_len != UNNAMED[i].len || memcmp(ram, buffer, ram_len) != 0) {
            fail(UNNAMED[i].what, "the bytes at 8 KiB taken out of the save");
        }
        free(ram);
        free(rtc);
        sigil_sync_result_free(u);
    }

    /* Data past the clock's place means the file isn't the core's buffer
     * with a clock: every byte stays in the save. */
    const uint32_t facts = SIGIL_FEATURE_GBA_EEPROM | SIGIL_FEATURE_RTC;
    buffer[100 * K] = 0x12;
    int rc;
    sigil_sync_result *u = collect_file("mgba", "Game.srm", buffer, sizeof(buffer), facts, &rc);
    size_t ram_len = 0, rtc_len = 0;
    uint8_t *ram = member_of(u, "save.sram", &ram_len);
    uint8_t *rtc = member_of(u, "clock.rtc", &rtc_len);
    if (rc != SIGIL_OK || rtc || !ram || ram_len != 128 * K || memcmp(ram, buffer, ram_len) != 0) {
        fail("data past the clock's place", "taken for a clock");
    }
    free(ram);
    free(rtc);
    sigil_sync_result_free(u);

    /* A save larger than the chip the ROM names fills the clock's place, so
     * restore over the core's buffer writes no clock into it. */
    static uint8_t big[32 * K];
    memset(big, 0x5A, sizeof(big));
    static const char *const NAMES[] = { "save.sram", "clock.rtc" };
    uint8_t *parts[2] = { big, g_clock };
    size_t lens[2] = { sizeof(big), 16 };
    size_t zip_len = 0;
    uint8_t *zip = legacy_zip(NAMES, parts, lens, 2, &zip_len);
    static uint8_t bare[128 * K];
    memset(bare, 0xFF, sizeof(bare));
    mem_root root = {0};
    root_put(&root, "Game.srm", bare, sizeof(bare));
    game g;
    make_game(&g, &root, "mgba", facts);
    sigil_sync_result *w = NULL;
    mem_file *f = NULL;
    if (!zip || sigil_restore(&g.req, zip, zip_len, &w) != SIGIL_OK || !(f = root_find(&root, "Game.srm")) ||
        f->len != 128 * K || memcmp(f->data, big, sizeof(big)) != 0 || !all_ff_tail(f->data, sizeof(big), 128 * K)) {
        fail("a 32 KiB save of an EEPROM cart", "the clock written over the save");
    }
    sigil_sync_result_free(w);
    root_free(&root);
    free(zip);
}

/* A zip of one emulator file, as clients uploaded one before sigil (RomM's
 * save.sram, an emulator's .srm or .sav), restores as the file itself does;
 * a zip holding anything else is refused and nothing is written. */
static void check_zips(void) {
    const chip *x = &g_chips[1];
    uint8_t source[VBA_NEXT_FILE + 16];
    size_t len = form_of(x, F_VBA_NEXT, false, source);
    uint8_t *data[1] = { source };
    size_t lens[1] = { len };
    static const char *const ONE[][1] = { { "save.sram" }, { "Game.srm" }, { "Game.sav" } };
    for (size_t i = 0; i < 3; i++) {
        size_t zip_len = 0;
        uint8_t *zip = legacy_zip(ONE[i], data, lens, 1, &zip_len);
        mem_root root = {0};
        game g;
        make_game(&g, &root, "gpsp", x->feature);
        size_t want = form_of(x, F_GPSP, false, g_want);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        if (!zip || sigil_restore(&g.req, zip, zip_len, &w) != SIGIL_OK || !(f = root_find(&root, "Game.srm")) ||
            f->len != want || memcmp(f->data, g_want, want) != 0) {
            fail(ONE[i][0], "a zip of VBA Next's file restores for gpSP as the file does");
        }
        sigil_sync_result_free(w);
        root_free(&root);
        free(zip);
    }
    static const char *const OTHER[] = { "notes.txt" };
    size_t zip_len = 0;
    uint8_t *other = legacy_zip(OTHER, data, lens, 1, &zip_len);
    mem_root root = {0};
    game g;
    make_game(&g, &root, "mgba", x->feature);
    sigil_sync_result *w = NULL;
    if (!other || sigil_restore(&g.req, other, zip_len, &w) != SIGIL_ERR_UNSUPPORTED_FORMAT || root.count) {
        fail("zip of notes.txt", "refused, nothing written");
    }
    sigil_sync_result_free(w);
    root_free(&root);
    free(other);

    /* RomM's save.sram and clock.rtc, the clock in another layout than
     * mGBA's 16 bytes: the save restores and the clock is left out. */
    static const char *const PAIR[] = { "save.sram", "clock.rtc" };
    uint8_t odd[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t *pair_data[2] = { (uint8_t *)x->bytes, odd };
    size_t pair_lens[2] = { x->len, sizeof(odd) };
    uint8_t *pair = legacy_zip(PAIR, pair_data, pair_lens, 2, &zip_len);
    mem_root timed = {0};
    make_game(&g, &timed, "mgba_standalone", x->feature | SIGIL_FEATURE_RTC);
    w = NULL;
    mem_file *f = NULL;
    if (!pair || sigil_restore(&g.req, pair, zip_len, &w) != SIGIL_OK || !(f = root_find(&timed, "Game.sav")) ||
        timed.count != 1 || f->len != x->len || memcmp(f->data, x->bytes, x->len) != 0) {
        fail("save.sram with an 8-byte clock.rtc", "the save doesn't restore without the clock");
    }
    sigil_sync_result_free(w);
    root_free(&timed);
    free(pair);
}

/* A local save that changed since the last sync isn't overwritten unasked;
 * the same save in another emulator's form is no conflict. */
static void check_conflict(void) {
    const chip *x = &g_chips[2];
    mem_root root = {0};
    uint8_t other[32 * K];
    memset(other, 0x11, sizeof(other));
    root_put(&root, "Game.srm", other, sizeof(other));
    game g;
    make_game(&g, &root, "mgba", x->feature);
    g.req.overwrite_local = 0;
    sigil_sync_result *w = NULL;
    if (sigil_restore(&g.req, x->bytes, x->len, &w) != SIGIL_ERR_CONFLICT || !w || !w->conflict) {
        fail("changed local save", "no conflict");
    }
    sigil_sync_result_free(w);
    root_free(&root);

    size_t len = form_of(x, F_GPSP, false, g_file);
    root_put(&root, "Game.srm", g_file, len);
    make_game(&g, &root, "gpsp", x->feature);
    g.req.overwrite_local = 0;
    w = NULL;
    if (sigil_restore(&g.req, x->bytes, x->len, &w) != SIGIL_OK || root.writes != 0) {
        fail("same save, gpSP's form", "a conflict or a rewrite");
    }
    sigil_sync_result_free(w);
    root_free(&root);
}

/* No libretro GBA core keeps the clock in a file of its own (mGBA's keeps it
 * in its save RAM), so locate expects no .rtc on GBA. */
static void check_locate(void) {
    mem_root root = {0};
    uint8_t ram[32 * K];
    memset(ram, 0, sizeof(ram));
    root_put(&root, "Game.srm", ram, sizeof(ram));
    game g;
    make_game(&g, &root, "mgba", SIGIL_FEATURE_RTC | SIGIL_FEATURE_GBA_FLASH1M);
    sigil_save_unit *u = NULL;
    if (sigil_save_resolve(&g.req.save, &u) != SIGIL_OK || u->member_count != 1 || u->expected_count != 0) {
        fail("gba clock cart locate", "a .rtc is expected");
    }
    sigil_save_unit_free(u);
    g.req.save.platform = "gb";
    u = NULL;
    if (sigil_save_resolve(&g.req.save, &u) != SIGIL_OK || u->expected_count != 1) {
        fail("gb clock cart locate", "the .rtc is no longer expected");
    }
    sigil_save_unit_free(u);
    root_free(&root);
}

int main(void) {
    make_chips();
    check_collect();
    check_restore();
    check_eeprom_size();
    check_retroarch_length();
    check_never_drops();
    check_clock();
    check_libretro_clock();
    check_zips();
    check_conflict();
    check_locate();
    printf("gba sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}

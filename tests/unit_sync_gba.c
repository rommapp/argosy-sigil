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
        if (c->eeprom) len = 8 * K;
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

/* EEPROM: 512 bytes unless a byte past them isn't 0xFF, whichever size the file came in. */
static void check_eeprom_size(void) {
    uint8_t small[8 * K];
    memset(small, 0xFF, sizeof(small));
    memcpy(small, g_chips[0].bytes, 512);
    int rc;
    sigil_sync_result *r = collect_file("vbam", "Game.srm", small, sizeof(small), SIGIL_FEATURE_GBA_EEPROM, &rc);
    if (rc != SIGIL_OK || !r || r->len != 512 || memcmp(r->data, small, 512) != 0) {
        fail("8 KiB EEPROM file, nothing past 512", "not 512 bytes");
    }
    sigil_sync_result_free(r);
    r = collect_file("vbam", "Game.srm", small, sizeof(small), 0, &rc);
    if (rc != SIGIL_OK || !r || r->len != 512) fail("8 KiB file, no facts, nothing past 512", "not 512 bytes");
    sigil_sync_result_free(r);
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

/* A clock cart: standalone mGBA's footer travels as clock.rtc, the identity
 * covers the chip alone, and only standalone mGBA gets the clock back. */
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

/* No libretro GBA core keeps the clock in a file, so locate expects no .rtc on GBA. */
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
    check_never_drops();
    check_clock();
    check_conflict();
    check_locate();
    printf("gba sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
